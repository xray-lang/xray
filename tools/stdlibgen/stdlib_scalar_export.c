/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_scalar_export.c - Build a typed native implementation from stdlib source
 */
#include "base/xresource_limits.h"
#include "lir/xlir_c.h"
#include "lir/xlir_lower.h"
#include "module/xmodule_resolver.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include <stdio.h>
#include <string.h>

static const char module_name[] = "math";
static const char module_path[] = "math/math.xr";
static const char export_name[] = "lerp";
static const char export_symbol[] = "xray_stdlib_math_lerp";

static bool same_name(const char *bytes, size_t length, const char *expected) {
    return bytes && length == strlen(expected) && !memcmp(bytes, expected, length);
}

static bool resolve_source(XrCompileResources *resources, const char *stdlib, XrModuleId *module) {
    XrModuleResolverConfig config = {.stdlib_path = stdlib};
    XrModuleResolver *resolver = NULL;
    char *expected = NULL;
    bool matches = xr_compile_module_resolver_new(resources, &config, &resolver) == XR_MODULE_OK &&
        xr_compile_module_resolver_resolve(resolver, module_name, NULL, NULL, module, NULL) == XR_MODULE_OK &&
        module->kind == XR_MOD_STDLIB && module->representation == XR_MODULE_SOURCE &&
        module->source_path && module->authority.kind == XR_MODULE_IDENTITY_STDLIB &&
        module->authority.physical_root && !strcmp(module->authority.namespace_id, module_name) &&
        !strcmp(module->logical_path, module_path) &&
        xr_compile_module_identity_from_logical(resources, &module->authority, module_path, &expected) ==
            XR_MODULE_OK && !strcmp(module->canonical, expected);
    xr_compile_resources_free(expected);
    xr_compile_module_resolver_free(resolver);
    return matches;
}

static uint32_t select_export(const XrXirArtifact *artifact, const XrModuleId *module) {
    const XrXirModule *view = xr_xir_compile_artifact_module(artifact);
    if (!view || !view->declarations || view->stage != XR_XIR_CHECKED ||
        !xr_xir_target_is_host(xr_xir_compile_artifact_target(artifact))) return UINT32_MAX;
    const XrXirDeclarations *declarations = view->declarations;
    uint32_t root = declarations->root_module;
    if (root >= declarations->module_count) return UINT32_MAX;
    const XrXirSourceModule *owner = &declarations->modules[root];
    if (!same_name(owner->name, owner->name_length, module->canonical)) return UINT32_MAX;
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < view->function_count; ++i) {
        const XrXirFunctionIdentity *identity = &declarations->functions[i];
        const XrXirFunction *body = &view->functions[i];
        if (identity->module != root || !identity->exported || identity->method_kind != XR_XIR_NON_MEMBER ||
            !same_name(body->name, body->name_length, export_name)) continue;
        if (selected != UINT32_MAX) return UINT32_MAX;
        selected = i;
    }
    return selected;
}

/* Match the build consumer's typed signature before any C name is assigned. */
static bool scalar_abi(const XrXirArtifact *artifact, uint32_t selected) {
    const XrXirModule *view = xr_xir_compile_artifact_module(artifact);
    const XrXirFunction *body = &view->functions[selected];
    const XrXirFunctionIdentity *identity = &view->declarations->functions[selected];
    return body->parameter_count == 3 && body->parameters[0] == XR_XIR_F64 &&
        body->parameters[1] == XR_XIR_F64 && body->parameters[2] == XR_XIR_F64 &&
        body->result == XR_XIR_F64 &&
        (body->error_type == XR_XIR_NEVER || body->error_type == XR_XIR_UNIT) &&
        body->block_count && !body->consume_parameter_count && !body->escaping_parameter_count &&
        !identity->native_callable && !identity->cleanup_owner && !identity->capture_owner &&
        (!view->generics || !view->generics[selected].parameter_count);
}

static bool closed_effects(const XrXirArtifact *artifact, uint32_t selected) {
    XrXirEffects *analysis = NULL;
    if (xr_xir_compile_effects_analyze(artifact, &analysis) != XR_XIR_OK) return false;
    XrXirEffectFacts facts = xr_xir_effects_read(analysis, selected);
    xr_xir_compile_effects_free(analysis);
    return facts.status == XR_XIR_OK && facts.control.suspend == XR_XIR_EFFECT_NONE &&
        facts.control.throws == XR_XIR_EFFECT_NONE && !facts.root.requires_root && !facts.root.unresolved;
}

static bool write_output(const char *path, const XrLirCOutput *output) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool written = fwrite(output->bytes, 1, output->length, file) == output->length;
    return fclose(file) == 0 && written;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    XrCompileResourceLimits limits = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    XrModuleId module = {0};
    XrCompilerSession *session = NULL;
    XrXirSourceResult source = {0};
    XrXirArtifact *closed = NULL;
    XrLirLowered lowered = {0};
    XrLirLowerFailure lower_failure = {0};
    XrLirCOutput output = {0};
    const char *error = "prepare compiler";
    int result = 1;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_session_new(resources, &session) != XR_COMPILE_RESOURCE_OK) goto done;

    error = "resolve canonical standard library source";
    if (!resolve_source(resources, argv[1], &module)) goto done;
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrXirSourceRequest request = {.session = session, .entry_path = module.source_path,
        .authority = &module.authority, .context = &context, .stdlib_path = argv[1],
        .linkage_kind = XR_XIR_PROGRAM, .target = xr_xir_target_host()};
    XrXirSourceDiagnostic check = {0};
    error = "check standard library source";
    if (xr_xir_compile_source_check(&request, &source, &check, NULL) != XR_XIR_OK) {
        fprintf(stderr, "%s:%d:%d: %s\n", module.source_path, check.line, check.column, check.message);
        goto done;
    }

    error = "select unique exported Checked math.lerp";
    uint32_t declaration = select_export(source.checked, &module);
    if (declaration == UINT32_MAX) goto done;
    error = "validate the declared scalar ABI";
    if (!scalar_abi(source.checked, declaration)) goto done;

    error = "specialize the Checked module";
    XrXirDiagnostic specialization = {0};
    if (xr_xir_compile_specialize(source.checked, &closed, &specialization, NULL) != XR_XIR_OK) goto done;
    error = "match the closed function to its original Checked declaration";
    uint32_t selected = select_export(closed, &module), origin = UINT32_MAX;
    if (selected == UINT32_MAX || !scalar_abi(closed, selected) ||
        xr_xir_compile_specialization_origin(closed, selected, &origin) != XR_XIR_OK ||
        origin != declaration) goto done;
    error = "validate closed scalar effects";
    if (!closed_effects(closed, selected)) goto done;

    error = "lower the selected Checked closure";
    XrLirLowerRequest lowering = {closed, source.snapshot, false};
    if (xr_lir_lower_stdlib_export_checked(&lowering, selected, &lowered, &lower_failure) != XR_LIR_OK) {
        fprintf(stderr, "%s\n", lower_failure.diagnostic.message);
        goto done;
    }
    error = "emit the verified scalar export";
    XrLirCDiagnostic c_diagnostic = {0};
    XrLirCRequest emission = {lowered.module, lowered.entry, resources, module.canonical, NULL};
    if (xr_lir_emit_c_scalar_export(&emission, export_symbol, &output, &c_diagnostic) != XR_LIR_C_OK) {
        fprintf(stderr, "%s\n", c_diagnostic.message);
        goto done;
    }
    error = "write the generated scalar export";
    if (write_output(argv[2], &output)) result = 0;

done:
    if (result) fprintf(stderr, "stdlib scalar export %s.%s: cannot %s\n", module_name, export_name, error);
    xr_lir_c_output_free(&output);
    xr_lir_lowered_free(&lowered);
    xr_lir_lower_failure_free(&lower_failure);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_module_id_cleanup(&module);
    xr_compile_resources_release(resources);
    return result;
}
