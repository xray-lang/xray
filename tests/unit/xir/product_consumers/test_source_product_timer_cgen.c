/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_timer_cgen.c - Public timer layout to owned C11 actions
 *
 * KEY CONCEPT:
 *   A verified operand layout determines the native duration load independently.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked inputs");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u, "Current target ABI");

static const char time_identity[] = "stdlib-module-v1:module=4:time:path=12:time/time.xr";
static const char symbol_prefix[] = "timer_cgen";

typedef struct TimerLayout {
    uint32_t function, instruction, operand, offset, frame_bytes, slots;
} TimerLayout;

static XrCompileResourceStats resource_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}

static const char *find_text(const char *text, size_t length, const char *needle) {
    size_t size = strlen(needle);
    CHECK(size > 0);
    if (size > length) return NULL;
    for (size_t i = 0; i <= length - size; ++i)
        if (!memcmp(text + i, needle, size)) return text + i;
    return NULL;
}

static const char *unique_text(const char *text, size_t length, const char *needle) {
    const char *found = find_text(text, length, needle);
    CHECK(found);
    size_t consumed = (size_t)(found - text) + strlen(needle);
    CHECK(consumed <= length && !find_text(text + consumed, length - consumed, needle));
    return found;
}

static void source_identity(const XrXirSourceProduct *product, const char *stdlib_root) {
    const XrXirSourceView *view = xr_xir_compile_source_product_view(product);
    const char suffix[] = "/time/time.xr";
    CHECK(view && view->complete && view->module_count == 2);
    uint32_t matches = 0;
    for (uint32_t m = 0; m < view->module_count; ++m) {
        const XrXirSourceQueryModule *module = &view->modules[m];
        CHECK(module->identity);
        if (strcmp(module->identity, time_identity)) continue;
        CHECK(!matches++ && module->path);
        size_t root = strlen(stdlib_root), length = strlen(module->path);
        CHECK(length == root + sizeof(suffix) - 1);
        for (size_t p = 0; p < length; ++p) {
            char expected = p < root ? stdlib_root[p] : suffix[p - root];
            char actual = module->path[p];
            CHECK((actual == '\\' ? '/' : actual) == (expected == '\\' ? '/' : expected));
        }
    }
    CHECK(matches == 1);
}

static TimerLayout timer_layout(const XrXirArtifact *lowered) {
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const XrXirTarget *target = xr_xir_compile_artifact_target(lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && target);
    CHECK(target->architecture == XR_XIR_ARCH_X86_64 && target->abi_version == XR_XIR_VALUE_ABI_VERSION);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && d->module_count == 2);
    uint32_t time_module = UINT32_MAX;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *owner = &d->modules[m];
        if (owner->name_length == sizeof(time_identity) - 1 &&
            !memcmp(owner->name, time_identity, sizeof(time_identity) - 1)) {
            CHECK(time_module == UINT32_MAX && m != d->root_module); time_module = m;
        }
    }
    CHECK(time_module != UINT32_MAX);
    TimerLayout result = {UINT32_MAX, UINT32_MAX, 0, 0, 0, 0};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_TIMER_AFTER_MS) continue;
            CHECK(result.function == UINT32_MAX && d->functions[f].module == time_module);
            CHECK(d->functions[f].exported && function->name_length == 5 && !memcmp(function->name, "sleep", 5));
            CHECK(function->parameter_count == 1 && function->parameters[0] == XR_XIR_I64);
            CHECK(function->result == XR_XIR_UNIT && op->type == XR_XIR_UNIT);
            CHECK(xr_xir_operand_type(function, op->args[0]) == XR_XIR_I64);
            CHECK(!op->args[1] && !op->targets[0] && !op->targets[1] && !op->immediate);
            const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(lowered, f);
            XrXirLayout scalar = {0};
            CHECK(layout && layout->offsets && op->args[0] < layout->slot_count);
            CHECK(xr_xir_builtin_layout(XR_XIR_I64, target, XR_XIR_LAYOUT_FRAME, &scalar) == XR_XIR_OK);
            CHECK(scalar.size == 8 && scalar.alignment == 8);
            uint32_t offset = layout->offsets[op->args[0]];
            CHECK(!(offset % scalar.alignment) && offset <= layout->frame_bytes);
            CHECK(scalar.size <= layout->frame_bytes - offset);
            result = (TimerLayout){f, i, op->args[0], offset, layout->frame_bytes, layout->slot_count};
        }
    }
    CHECK(result.function != UINT32_MAX);
    printf("timer-cgen-layout function=%u instruction=%u operand=%u offset=%u frame-bytes=%u slots=%u\n",
        result.function, result.instruction, result.operand, result.offset, result.frame_bytes, result.slots);
    return result;
}

static XrXirStatus build_checked(const char *root, const char *entry, const char *stdlib_root,
                                XrXirCompileContext *context, XrXirArtifact **checked) {
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {
        {session, entry, &authority, context, stdlib_root, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    if (status == XR_XIR_OK) {
        CHECK(xr_xir_compile_source_product_context(product)->resources == context->resources);
        source_identity(product, stdlib_root);
        XrXirSourceProductPacketView packet = {0};
        status = xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet);
        if (status == XR_XIR_OK) {
            CHECK(packet.bytes && packet.length);
            status = xr_xir_compile_checked_read(context, packet.bytes, packet.length, checked, NULL);
        }
    } else {
        CHECK(!product);
        fprintf(stderr, "timer-cgen-source status=%u stage=%u source-status=%u module=%u "
            "line=%d column=%d message=%s\n", (unsigned)status, (unsigned)diagnostic.stage,
            (unsigned)diagnostic.source.status, diagnostic.source.module, diagnostic.source.line,
            diagnostic.source.column, diagnostic.source.message);
        if (diagnostic.source_path) fprintf(stderr, "source-path=%s\n", diagnostic.source_path);
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_session_free(session);
    printf("timer-cgen-source-owner-destroyed status=%u checked-owned=%u\n", (unsigned)status, *checked ? 1u : 0u);
    return status;
}

static XrXirStatus emit_owned(const XrXirArtifact *lowered, unsigned repeat, XrXirCSource *output) {
    XrCompileResources *resources = xr_xir_compile_artifact_context(lowered)->resources;
    XrCompileResourceStats before = resource_stats(resources);
    size_t attempts = instance_compile_attempts;
    XrXirStatus status = xr_xir_compile_emit_c(lowered, symbol_prefix, UINT64_C(16777216), output);
    XrCompileResourceStats after = resource_stats(resources);
    printf("timer-cgen-emission repeat=%u status=%u attempts=%zu allocations=%" PRIu64
        " allocated-bytes=%" PRIu64 " live-before=%" PRIu64 " live-after=%" PRIu64 " work=%" PRIu64 "\n",
        repeat, (unsigned)status, instance_compile_attempts - attempts,
        after.allocation_count - before.allocation_count, after.allocated_bytes - before.allocated_bytes,
        before.live_bytes, after.live_bytes, after.work - before.work);
    if (status == XR_XIR_OK) {
        CHECK(output->text && output->length && after.live_bytes > before.live_bytes);
        CHECK(instance_compile_attempts > attempts && after.work > before.work);
    } else CHECK(!output->text && !output->length && after.live_bytes == before.live_bytes);
    return status;
}

static void generated_timer(const XrXirCSource *source, const TimerLayout *timer) {
    char owner[96], callback[128], expected[256], instruction[96];
    int n = snprintf(owner, sizeof(owner), "typedef struct %s_state_%u {\n", symbol_prefix, timer->function);
    CHECK(n > 0 && (size_t)n < sizeof(owner));
    const char *first = unique_text(source->text, source->length, owner);
    size_t remaining = source->length - (size_t)(first - source->text);
    const char *next_owner = find_text(first + strlen(owner), remaining - strlen(owner),
        "\ntypedef struct timer_cgen_state_");
    const char *entries = find_text(first, remaining,
        "\nXR_DATADEF const XrXirCallEntry timer_cgen_entries[] = {");
    CHECK(entries);
    const char *end = next_owner && next_owner < entries ? next_owner : entries;
    size_t section = (size_t)(end - first);
    n = snprintf(callback, sizeof(callback), "XR_FUNC XrXirAction %s_f%u(XrXirCallView *view) {\n",
        symbol_prefix, timer->function);
    CHECK(n > 0 && (size_t)n < sizeof(callback));
    (void)unique_text(first, section, callback);
    n = snprintf(expected, sizeof(expected), "return (XrXirAction) {XR_XIR_ACTION_TIMER, 0, NULL, 0, "
        "{XR_XIR_I64, 0, xr_xir_scalar_load(state->frame, %uu)}, {0}, 0};", timer->offset);
    CHECK(n > 0 && (size_t)n < sizeof(expected));
    const char *action = unique_text(source->text, source->length, "XR_XIR_ACTION_TIMER");
    const char *typed = unique_text(first, section, expected);
    CHECK(action >= typed && action < typed + strlen(expected));
    n = snprintf(instruction, sizeof(instruction), "    case %uu:\n        state->pc = %uu;\n",
        timer->instruction, timer->instruction + 1);
    CHECK(n > 0 && (size_t)n < sizeof(instruction));
    const char *case_start = unique_text(first, section, instruction);
    size_t case_remaining = (size_t)(end - case_start);
    const char *next_case = find_text(case_start + strlen(instruction), case_remaining - strlen(instruction), "\n    case ");
    const char *fallback = find_text(case_start + strlen(instruction), case_remaining - strlen(instruction), "\n    default:");
    CHECK(fallback);
    const char *case_end = next_case && next_case < fallback ? next_case : fallback;
    CHECK(typed > case_start && typed + strlen(expected) <= case_end);
    printf("timer-cgen-mapping owner=time.sleep function=%u instruction=%u action=TIMER type=I64 "
        "operand=%u frame-offset=%u A38-A39=METADATA_MATCH A40=EXECUTION_EVIDENCE_SEPARATE\n",
        timer->function, timer->instruction, timer->operand, timer->offset);
}

int main(int argc, char **argv) {
    CHECK(argc == 4);
    instance_compile_zero();
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirCSource first = {0}, repeated = {0};
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = resource_stats(context.resources);
    XrXirStatus status = build_checked(argv[1], argv[2], argv[3], &context, &checked);
    const char *operation = "source-product";
    int passed = 0;
    if (status != XR_XIR_OK) goto release;
    operation = "checked-verify";
    status = xr_xir_compile_artifact_verify(checked, &diagnostic);
    if (status != XR_XIR_OK) goto release;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    operation = "lower";
    status = xr_xir_compile_lower(checked, &target, &lowered, &diagnostic);
    if (status != XR_XIR_OK) goto release;
    operation = "lowered-verify";
    status = xr_xir_compile_artifact_verify(lowered, &diagnostic);
    if (status != XR_XIR_OK) goto release;
    TimerLayout timer = timer_layout(lowered);
    operation = "emit-first";
    status = emit_owned(lowered, 0, &first);
    if (status != XR_XIR_OK) goto release;
    operation = "emit-repeat";
    status = emit_owned(lowered, 1, &repeated);
    if (status != XR_XIR_OK) goto release;
    CHECK(first.length == repeated.length && !memcmp(first.text, repeated.text, first.length));
    xr_xir_compile_artifact_free(lowered); lowered = NULL;
    xr_xir_compile_artifact_free(checked); checked = NULL;
    printf("timer-cgen-artifact-owners-destroyed C-sources-owned=2 bytes=%zu\n", first.length);
    generated_timer(&first, &timer); generated_timer(&repeated, &timer);
    passed = 1;
release:
    if (status != XR_XIR_OK)
        fprintf(stderr, "timer-cgen-projection operation=%s status=%u function=%u block=%u "
            "instruction=%u reason=%u\n", operation, (unsigned)status, diagnostic.function,
            diagnostic.block, diagnostic.instruction, (unsigned)diagnostic.reason);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(checked);
    xr_xir_compile_c_source_free(&first); xr_xir_compile_c_source_free(&repeated);
    CHECK(!first.text && !first.length && !repeated.text && !repeated.length);
    XrCompileResourceStats final = resource_stats(context.resources);
    CHECK(final.live_bytes == baseline.live_bytes);
    printf("timer-cgen-final allocated-bytes=%" PRIu64 " live-bytes=%" PRIu64 " work=%" PRIu64
        " compiler-attempts=%zu\n", final.allocated_bytes, final.live_bytes, final.work, instance_compile_attempts);
    xr_compile_resources_release(context.resources);
    instance_compile_report();
    printf("timer-cgen-metadata result=%s runtime/native/HostWait=NOT_RUN\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
