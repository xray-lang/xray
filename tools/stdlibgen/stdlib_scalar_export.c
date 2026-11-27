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
#include "xir/xxir_builtin_stdlib.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif

static const char module_name[] = "math";
static const char module_path[] = "math/math.xr";

typedef struct ScalarExport {
    const char *name, *symbol;
    XrXirBuiltinScalarAbi abi;
} ScalarExport;

enum { MATH_SCALAR_EXPORT_COUNT = 2 };
static const ScalarExport scalar_exports[MATH_SCALAR_EXPORT_COUNT] = {
    {"lerp", "xray_stdlib_math_lerp", XR_XIR_BUILTIN_F64_TERNARY},
    {"abs", "xray_stdlib_math_abs", XR_XIR_BUILTIN_F64_UNARY}
};

typedef struct ScalarOutput {
    const ScalarExport *spec;
    const char *destination;
    uint32_t checked_index, closed_index;
    XrLirCOutput c;
} ScalarOutput;

static bool same_name(const char *bytes, size_t length, const char *expected) {
    return bytes && expected && length == strlen(expected) && !memcmp(bytes, expected, length);
}

#include "stdlib_checked_interface.inc.c"

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

static uint32_t select_export(const XrXirArtifact *artifact, const XrModuleId *module, const char *name) {
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
            !same_name(body->name, body->name_length, name)) continue;
        if (selected != UINT32_MAX) return UINT32_MAX;
        selected = i;
    }
    return selected;
}

/* Match the build consumer's typed signature before any C name is assigned. */
static bool scalar_abi(const XrXirArtifact *artifact, uint32_t selected, const ScalarExport *spec) {
    const XrXirModule *view = xr_xir_compile_artifact_module(artifact);
    const XrXirFunction *body = &view->functions[selected];
    const XrXirFunctionIdentity *identity = &view->declarations->functions[selected];
    if (body->parameter_count != (uint32_t)spec->abi) return false;
    for (uint32_t p = 0; p < body->parameter_count; ++p)
        if (body->parameters[p] != XR_XIR_F64) return false;
    return body->result == XR_XIR_F64 &&
        (body->error_type == XR_XIR_NEVER || body->error_type == XR_XIR_UNIT) &&
        body->block_count && !body->consume_parameter_count && !body->escaping_parameter_count &&
        !identity->native_callable && !identity->cleanup_owner && !identity->capture_owner &&
        (!view->generics || !view->generics[selected].parameter_count);
}

static bool scalar_effects_allowed(const XrXirEffects *analysis, uint32_t selected) {
    XrXirEffectFacts facts = xr_xir_effects_read(analysis, selected);
    return facts.status == XR_XIR_OK && facts.control.suspend == XR_XIR_EFFECT_NONE &&
        facts.control.throws == XR_XIR_EFFECT_NONE && !facts.root.requires_root && !facts.root.unresolved;
}

static FILE *open_output(XrCompileResources *resources, const char *path) {
#ifdef XR_OS_WINDOWS
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    wchar_t *wide_path = NULL;
    if (xr_win_utf8_path_owned(&policy, path, &wide_path) != XR_OS_IO_OK) return NULL;
    FILE *file = _wfopen(wide_path, L"wb");
    xr_compile_resources_free(wide_path);
    return file;
#else
    (void) resources;
    return fopen(path, "wb");
#endif
}

static bool write_output(XrCompileResources *resources, const ScalarOutput *output) {
    FILE *file = open_output(resources, output->destination);
    if (!file) return false;
    bool written = fwrite(output->c.bytes, 1, output->c.length, file) == output->c.length;
    return fclose(file) == 0 && written;
}

static const char *emit_scalar(XrCompileResources *resources, const XrXirSourceSnapshot *snapshot,
    const XrXirArtifact *closed, const XrModuleId *module, ScalarOutput *output) {
    XrLirLowered lowered = {0};
    XrLirLowerFailure lower_failure = {0};
    XrLirLowerRequest lowering = {closed, snapshot, false};
    if (xr_lir_lower_stdlib_export_checked(&lowering, output->closed_index, &lowered, &lower_failure) != XR_LIR_OK) {
        fprintf(stderr, "%s\n", lower_failure.diagnostic.message);
        xr_lir_lowered_free(&lowered);
        xr_lir_lower_failure_free(&lower_failure);
        return "lower the selected Checked closure";
    }
    XrLirCDiagnostic diagnostic = {0};
    XrLirCRequest emission = {lowered.module, lowered.entry, resources, module->canonical, NULL};
    XrLirCStatus status = xr_lir_emit_c_scalar_export(&emission, output->spec->symbol, &output->c, &diagnostic);
    xr_lir_lowered_free(&lowered);
    if (status == XR_LIR_C_OK) return NULL;
    fprintf(stderr, "%s\n", diagnostic.message);
    return "emit the verified scalar export";
}

static int produce(const char *stdlib, ScalarOutput *outputs, uint32_t count, const char *interface_destination) {
    XrCompileResourceLimits limits = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    XrModuleId module = {0};
    XrCompilerSession *session = NULL;
    XrXirSourceResult source = {0};
    XrXirArtifact *closed = NULL;
    XrXirEffects *analysis = NULL;
    CheckedMathInterface projection = {0};
    const char *subject = count == 1 ? outputs[0].spec->name : NULL;
    const char *error = "prepare compiler";
    int result = 1;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_session_new(resources, &session) != XR_COMPILE_RESOURCE_OK) goto done;

    error = "resolve canonical standard library source";
    if (!resolve_source(resources, stdlib, &module)) goto done;
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrXirSourceRequest request = {.session = session, .entry_path = module.source_path,
        .authority = &module.authority, .context = &context, .stdlib_path = stdlib,
        .linkage_kind = XR_XIR_PROGRAM, .target = xr_xir_target_host()};
    XrXirSourceDiagnostic check = {0};
    error = "check standard library source";
    if (xr_xir_compile_source_check(&request, &source, &check, NULL) != XR_XIR_OK) {
        fprintf(stderr, "%s:%d:%d: %s\n", module.source_path, check.line, check.column, check.message);
        goto done;
    }

    for (uint32_t i = 0; i < count; ++i) {
        ScalarOutput *output = &outputs[i];
        subject = output->spec->name;
        error = "select the unique exported Checked declaration";
        output->checked_index = select_export(source.checked, &module, subject);
        if (output->checked_index == UINT32_MAX) goto done;
        error = "validate the declared scalar ABI";
        if (!scalar_abi(source.checked, output->checked_index, output->spec)) goto done;
    }
    if (count != 1) subject = NULL;
    error = "specialize the Checked module";
    XrXirDiagnostic specialization = {0};
    if (xr_xir_compile_specialize(source.checked, &closed, &specialization, NULL) != XR_XIR_OK) goto done;
    error = "analyze the closed scalar effects";
    if (xr_xir_compile_effects_analyze(closed, &analysis) != XR_XIR_OK) goto done;

    for (uint32_t i = 0; i < count; ++i) {
        ScalarOutput *output = &outputs[i];
        subject = output->spec->name;
        error = "match the closed function to its original Checked declaration";
        output->closed_index = select_export(closed, &module, subject);
        uint32_t origin = UINT32_MAX;
        if (output->closed_index == UINT32_MAX || !scalar_abi(closed, output->closed_index, output->spec) ||
            xr_xir_compile_specialization_origin(closed, output->closed_index, &origin) != XR_XIR_OK ||
            origin != output->checked_index) goto done;
        error = "validate closed scalar effects";
        if (!scalar_effects_allowed(analysis, output->closed_index)) goto done;
        error = emit_scalar(resources, source.snapshot, closed, &module, output);
        if (error) goto done;
    }
    if (interface_destination) {
        subject = NULL;
        error = "project the complete dependency-free Checked interface";
        if (!prepare_interface(&source, outputs, &projection)) goto done;
    }

    /* Validate every artifact before writing any pending build output. */
    for (uint32_t i = 0; i < count; ++i) {
        subject = outputs[i].spec->name;
        error = "write the generated scalar export";
        if (!write_output(resources, &outputs[i])) goto done;
    }
    if (interface_destination) {
        subject = NULL;
        error = "write the generated Checked interface";
        FILE *file = open_output(resources, interface_destination);
        if (!file) goto done;
        write_interface(file, &projection, xr_xir_compile_artifact_target(source.checked));
        bool ok = !ferror(file);
        if (fclose(file) || !ok) goto done;
    }
    result = 0;

done:
    if (result) fprintf(stderr, "stdlib scalar export %s%s%s: cannot %s\n", module_name,
        subject ? "." : "", subject ? subject : "", error);
    for (uint32_t i = 0; i < count; ++i) xr_lir_c_output_free(&outputs[i].c);
    xr_xir_compile_effects_free(projection.effects);
    xr_xir_compile_effects_free(analysis);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_module_id_cleanup(&module);
    xr_compile_resources_release(resources);
    return result;
}

static int run_producer(int argc, char **argv) {
    if ((argc == 5 || argc == 6) && !strcmp(argv[2], "--bundle")) {
        ScalarOutput outputs[MATH_SCALAR_EXPORT_COUNT] = {
            {.spec = &scalar_exports[0], .destination = argv[3]},
            {.spec = &scalar_exports[1], .destination = argv[4]}
        };
        return produce(argv[1], outputs, MATH_SCALAR_EXPORT_COUNT, argc == 6 ? argv[5] : NULL);
    }
    if (argc != 3 && argc != 4) return 2;
    const char *name = argc == 4 ? argv[3] : "lerp";
    for (size_t i = 0; i < MATH_SCALAR_EXPORT_COUNT; ++i) {
        if (!strcmp(name, scalar_exports[i].name)) {
            ScalarOutput output = {.spec = &scalar_exports[i], .destination = argv[2]};
            return produce(argv[1], &output, 1, NULL);
        }
    }
    fputs("stdlib scalar export: unknown export selection\n", stderr);
    return 2;
}

#ifdef XR_OS_WINDOWS
int wmain(int argc, wchar_t **wide_argv) {
    if (argc < 3 || argc > 6) return 2;
    XrWinPathStatus status;
    char **argv = xr_win_utf16_arguments(argc, wide_argv, &status);
    if (!argv) {
        fputs("stdlib scalar export: invalid Windows arguments or argument allocation failed\n", stderr);
        return 1;
    }
    int result = run_producer(argc, argv);
    xr_win_utf8_arguments_free(argc, argv);
    return result;
}
#else
int main(int argc, char **argv) {
    return run_producer(argc, argv);
}
#endif
