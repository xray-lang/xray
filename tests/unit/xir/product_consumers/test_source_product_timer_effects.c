/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_timer_effects.c - Detached public timer effect witnesses
 *
 * KEY CONCEPT:
 *   Public facts identify the actual source owner and a finite typed call chain.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_effects.h"
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

typedef struct TimerRoots {
    uint32_t answer, consumer_answer, time_module, timer_function, timer_instruction;
} TimerRoots;

static bool named(const XrXirFunction *function, const char *name) {
    size_t length = strlen(name);
    return function->name_length == length && !memcmp(function->name, name, length);
}

static XrCompileResourceStats resource_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
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

static TimerRoots identify_roots(const XrXirModule *module) {
    TimerRoots roots = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && d->module_count == 2 && d->root_module < d->module_count);
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *owner = &d->modules[m];
        if (owner->name_length == sizeof(time_identity) - 1 &&
            !memcmp(owner->name, time_identity, sizeof(time_identity) - 1)) {
            CHECK(roots.time_module == UINT32_MAX && m != d->root_module);
            roots.time_module = m;
        }
    }
    CHECK(roots.time_module != UINT32_MAX);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module == d->root_module && named(function, "answer")) {
            CHECK(roots.answer == UINT32_MAX && !identity->exported);
            CHECK(!function->parameter_count && function->result == XR_XIR_I64);
            roots.answer = f;
        }
        if (identity->module == d->root_module && named(function, "consumerAnswer")) {
            CHECK(roots.consumer_answer == UINT32_MAX && identity->exported);
            CHECK(!function->parameter_count && function->result == XR_XIR_I64);
            roots.consumer_answer = f;
        }
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_TIMER_AFTER_MS) continue;
            CHECK(roots.timer_function == UINT32_MAX && identity->module == roots.time_module);
            CHECK(identity->exported && named(function, "sleep"));
            CHECK(function->parameter_count == 1 && function->parameters[0] == XR_XIR_I64);
            CHECK(function->result == XR_XIR_UNIT && op->type == XR_XIR_UNIT);
            CHECK(xr_xir_operand_type(function, op->args[0]) == XR_XIR_I64);
            CHECK(!op->args[1] && !op->targets[0] && !op->targets[1] && !op->immediate);
            roots.timer_function = f; roots.timer_instruction = i;
        }
    }
    CHECK(roots.answer != UINT32_MAX && roots.consumer_answer != UINT32_MAX);
    CHECK(roots.answer != roots.consumer_answer && roots.timer_function != UINT32_MAX);
    return roots;
}

static void witness_chain(const char *stage, const XrXirModule *module, const XrXirEffects *effects,
                          const TimerRoots *roots, uint32_t root) {
    uint32_t function = root;
    for (uint32_t step = 0; step < module->function_count; ++step) {
        const XrXirFunction *fn = &module->functions[function];
        const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, function);
        const XrXirEffectWitness *witness = xr_xir_effects_suspend_witness(effects, function);
        CHECK(fact && fact->suspend == XR_XIR_EFFECT_MAY && fact->throws == XR_XIR_EFFECT_NONE);
        CHECK(witness && witness->instruction < fn->instruction_count);
        const XrXirInstruction *op = &fn->instructions[witness->instruction];
        printf("timer-effect-witness stage=%s root=%u step=%u function=%u instruction=%u "
            "cause=%u distance=%u callee=%u op=%s\n", stage, root, step, function,
            witness->instruction, (unsigned)witness->cause, witness->distance, witness->callee,
            xr_xir_op_name(op->op));
        if (witness->cause == XR_XIR_EFFECT_CAUSE_SUSPEND) {
            CHECK(!witness->distance && witness->callee == UINT32_MAX);
            CHECK(function == roots->timer_function && witness->instruction == roots->timer_instruction);
            CHECK(module->declarations->functions[function].module == roots->time_module);
            CHECK(op->op == XR_XIR_TIMER_AFTER_MS && op->type == XR_XIR_UNIT);
            CHECK(step > 0);
            return;
        }
        CHECK(witness->cause == XR_XIR_EFFECT_CAUSE_CALL && witness->distance > 0);
        CHECK(witness->callee < module->function_count && op->op == XR_XIR_CALL);
        CHECK(op->immediate >= 0 && (uint64_t)op->immediate == witness->callee);
        const XrXirEffectWitness *next = xr_xir_effects_suspend_witness(effects, witness->callee);
        CHECK(next && next->distance == witness->distance - 1);
        if (!step && root == roots->answer) CHECK(witness->callee == roots->timer_function);
        if (!step && root == roots->consumer_answer) CHECK(witness->callee == roots->answer);
        function = witness->callee;
    }
    CHECK(false);
}

static bool analyze_stage(const char *stage, XrXirStage expected, const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    CHECK(module && module->stage == expected && context);
    TimerRoots roots = identify_roots(module);
    XrXirEffects *effects = NULL;
    XrCompileResourceStats before = resource_stats(context->resources);
    size_t blocks = instance_compile_live, bytes = instance_compile_bytes;
    size_t attempts = instance_compile_attempts;
    XrXirStatus status = xr_xir_compile_effects_analyze(artifact, &effects);
    XrCompileResourceStats analyzed = resource_stats(context->resources);
    size_t analysis_attempts = instance_compile_attempts - attempts;
    printf("timer-effects-analysis stage=%s status=%u compiler-attempts=%zu allocations=%" PRIu64
        " allocated-bytes=%" PRIu64 " work=%" PRIu64 "\n", stage, (unsigned)status, analysis_attempts,
        analyzed.allocation_count - before.allocation_count, analyzed.allocated_bytes - before.allocated_bytes,
        analyzed.work - before.work);
    if (status != XR_XIR_OK) {
        CHECK(!effects);
        CHECK(analyzed.live_bytes == before.live_bytes);
        CHECK(instance_compile_live == blocks && instance_compile_bytes == bytes);
        return false;
    }
    CHECK(effects && analysis_attempts > 0 && analyzed.work > before.work);
    size_t query_attempts = instance_compile_attempts;
    uint32_t functions[] = {roots.answer, roots.consumer_answer};
    for (unsigned i = 0; i < 2; ++i) {
        const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, functions[i]);
        CHECK(fact && fact->suspend == XR_XIR_EFFECT_MAY && fact->throws == XR_XIR_EFFECT_NONE);
        printf("timer-effects-root stage=%s name=%s function=%u suspend=MAY throws=NONE\n",
            stage, i ? "consumerAnswer" : "answer", functions[i]);
        witness_chain(stage, module, effects, &roots, functions[i]);
    }
    CHECK(instance_compile_attempts == query_attempts);
    xr_xir_compile_effects_free(effects);
    XrCompileResourceStats released = resource_stats(context->resources);
    CHECK(released.live_bytes == before.live_bytes);
    CHECK(instance_compile_live == blocks && instance_compile_bytes == bytes);
    printf("timer-effects-release stage=%s summary-blocks/bytes=0/0\n", stage);
    return true;
}

int main(int argc, char **argv) {
    CHECK(argc == 4);
    instance_compile_zero();
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirDiagnostic xir = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = resource_stats(context.resources);
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[1]};
    XrXirSourceProductRequest request = {
        {session, argv[2], &authority, &context, argv[3], NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    const char *operation = "source-product";
    int passed = 0;
    if (status != XR_XIR_OK) {
        CHECK(!product);
        fprintf(stderr, "timer-effects-source status=%u stage=%u source-status=%u module=%u "
            "line=%d column=%d message=%s\n", (unsigned)status, (unsigned)diagnostic.stage,
            (unsigned)diagnostic.source.status, diagnostic.source.module, diagnostic.source.line,
            diagnostic.source.column, diagnostic.source.message);
        if (diagnostic.source_path) fprintf(stderr, "source-path=%s\n", diagnostic.source_path);
        goto release;
    }
    CHECK(xr_xir_compile_source_product_context(product)->resources == context.resources);
    source_identity(product, argv[3]);
    XrXirSourceProductPacketView packet = {0};
    operation = "closed-packet";
    status = xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet);
    if (status != XR_XIR_OK) goto release;
    CHECK(packet.bytes && packet.length);
    operation = "checked-read";
    status = xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, &xir);
    if (status != XR_XIR_OK) goto release;
    xr_compile_session_free(session); session = NULL;
    xr_xir_compile_source_product_free(product); product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    operation = "checked-verify";
    status = xr_xir_compile_artifact_verify(checked, &xir);
    if (status != XR_XIR_OK) goto release;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    operation = "lower";
    status = xr_xir_compile_lower(checked, &target, &lowered, &xir);
    if (status != XR_XIR_OK) goto release;
    operation = "lowered-verify";
    status = xr_xir_compile_artifact_verify(lowered, &xir);
    if (status != XR_XIR_OK) goto release;
    if (!analyze_stage("Checked", XR_XIR_CHECKED, checked)) goto release;
    if (!analyze_stage("Lowered", XR_XIR_LOWERED, lowered)) goto release;
    passed = 1;
release:
    if (status != XR_XIR_OK)
        fprintf(stderr, "timer-effects-projection operation=%s status=%u function=%u block=%u "
            "instruction=%u reason=%u\n", operation, (unsigned)status, xir.function, xir.block,
            xir.instruction, (unsigned)xir.reason);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(checked);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_session_free(session);
    XrCompileResourceStats final = resource_stats(context.resources);
    CHECK(final.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    instance_compile_report();
    printf("timer-effects-metadata result=%s compiler-attempts=%zu runtime-execution=NOT_RUN\n",
        passed ? "PASS" : "FAIL", instance_compile_attempts);
    return passed ? 0 : 1;
}
