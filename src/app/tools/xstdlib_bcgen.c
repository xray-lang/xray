/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_bcgen.c - build-time stdlib bytecode compiler
 *
 * KEY CONCEPT:
 *   The final runtime embeds stdlib bytecode, but producing those build-private
 *   files needs a compiler. This tiny build-only tool links against a bootstrap
 *   core that has embedded source plus an empty bytecode table, avoiding a
 *   final-runtime self-dependency cycle. The hosted-fragment command also
 *   supplies generic source-to-C fixtures without installing VM exports.
 */

#include "../../api/xisolate_profile.h"
#include "../../aot/xaot_driver.h"
#include "../../base/xmalloc.h"
#include "../../module/xproto_codec.h"
#include "../../module/xmodule_identity.h"
#include "../../module/xproject.h"
#include "../../runtime/xr_process_shutdown.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../frontend/codegen/xcompiler_context.h"
#include "../../runtime/xisolate_internal.h"
#include "../../plan/target/xr_target_profile.h"
#include "../toolchain/xtc_target_profile.h"
#include "xray_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr, "usage:\n"
                    "  xray_stdlib_bcgen compile <input.xr> --output <bytecode-output> "
                    "--format bytecode [--stdlib-module <canonical-name>] "
                    "[--strip-debug] [--strip-source]\n"
                    "  xray_stdlib_bcgen hosted-fragment <input.xr> --output <output.c> "
                    "--header <output.h>\n");
    return 2;
}

static bool write_bytes(const char *path, const char *bytes, size_t size) {
    FILE *out = path ? fopen(path, "wb") : NULL;
    bool ok = out && fwrite(bytes, 1, size, out) == size;
    if (out && fclose(out) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "xray_stdlib_bcgen: cannot write '%s'\n", path ? path : "<null>");
    return ok;
}

static int generate_hosted_fragment(int argc, char **argv) {
    if (argc < 5)
        return usage();
    const char *input = argv[2];
    const char *output = NULL;
    const char *header = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header = argv[++i];
            continue;
        }
        fprintf(stderr, "xray_stdlib_bcgen: unknown hosted-fragment argument '%s'\n", argv[i]);
        return 2;
    }
    if (!output || !header)
        return usage();

    XaotTarget target;
    XaotBuildOptions options;
    XaotBuildResult result;
    XrModuleIdentityAuthority entry_authority = {0};
    char *entry_authority_namespace = NULL;
    char *entry_authority_root = NULL;
    XrTargetProfile *target_profile = NULL;
    XrProject *project = xr_project_load(NULL, ".");
    if (!project || !project->initialized || !project->native_plan) {
        fprintf(stderr, "xray_stdlib_bcgen: invalid hosted fragment manifest%s%s\n",
                project && project->native_plan && project->native_plan->error ? ": " : "",
                project && project->native_plan && project->native_plan->error
                    ? project->native_plan->error
                    : "");
        xr_project_free(project);
        return 1;
    }
    if (!xaot_target_init(&target, "native-c90")) {
        fprintf(stderr, "xray_stdlib_bcgen: cannot initialize hosted AOT target\n");
        xr_project_free(project);
        return 1;
    }
    memset(&options, 0, sizeof(options));
    XrTargetCodegenFacts codegen;
    char profile_error[256];
    if (!xaot_target_profile_codegen_facts(&target, &codegen) ||
        !xtc_target_profile_build_current_native_hosted(&codegen, &target_profile, profile_error,
                                                        sizeof(profile_error))) {
        fprintf(stderr, "xray_stdlib_bcgen: %s\n", profile_error);
        xaot_target_free(&target);
        xr_project_free(project);
        return 1;
    }
    options.target = &target;
    options.target_profile = target_profile;
    options.native_package_plan = project->native_plan;
    char authority_err[256];
    if (!xr_project_module_identity_authority(project, &entry_authority, &entry_authority_namespace,
                                              &entry_authority_root, authority_err,
                                              sizeof(authority_err))) {
        fprintf(stderr, "Error: invalid hosted fragment module identity authority: %s\n",
                authority_err);
        xr_target_profile_free(target_profile);
        xaot_target_free(&target);
        xr_project_free(project);
        return 1;
    }
    options.entry_module_authority = entry_authority;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    options.c_dialect = XI_CGEN_C_DIALECT_C11;
    options.type_name_profile = XI_CGEN_TYPE_NAMES_NONE;
    options.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    options.quiet = true;

    int rc = xaot_build(input, &options, &result);
    xr_free(entry_authority_namespace);
    xr_free(entry_authority_root);
    xr_target_profile_free(target_profile);
    xaot_target_free(&target);
    if (rc != 0) {
        xr_project_free(project);
        return rc;
    }

    size_t c_size = 0;
    char *c_source = xaot_build_result_amalgamate(&result, &c_size);
    const char *header_source = result.c_export_header;
    bool ok = c_source && header_source && write_bytes(output, c_source, c_size) &&
              write_bytes(header, header_source, strlen(header_source));
    xr_free(c_source);
    xaot_build_result_free(&result);
    xr_project_free(project);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    atexit(xr_process_shutdown);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "hosted-fragment") == 0)
        return generate_hosted_fragment(argc, argv);
    if (argc < 3 || strcmp(argv[1], "compile") != 0)
        return usage();

    const char *input = argv[2];
    const char *output = NULL;
    const char *stdlib_module = NULL;
    int flags = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc)
                return usage();
            output = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc)
                return usage();
            const char *fmt = argv[++i];
            if (strcmp(fmt, "bytecode") != 0) {
                fprintf(stderr, "xray_stdlib_bcgen: unsupported format '%s'\n", fmt);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--stdlib-module") == 0) {
            if (i + 1 >= argc)
                return usage();
            stdlib_module = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--strip-debug") == 0) {
            flags |= XR_BOOTSTRAP_CONTAINER_STRIP_DEBUG;
            continue;
        }
        if (strcmp(argv[i], "--strip-source") == 0) {
            flags |= XR_BOOTSTRAP_CONTAINER_STRIP_SOURCE;
            continue;
        }
        fprintf(stderr, "xray_stdlib_bcgen: unknown argument '%s'\n", argv[i]);
        return 2;
    }

    if (!output)
        return usage();

    XrVMRuntime *X = xr_isolate_profile_new(XR_ISOLATE_PROFILE_RUN);
    if (!X) {
        fprintf(stderr, "xray_stdlib_bcgen: failed to create compiler isolate\n");
        return 1;
    }

    XrCompilerSession *session = X->compiler_session;
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) {
        xray_vm_delete(X);
        return 1;
    }
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(X->native_target_profile);
    if (!machine) {
        xray_vm_delete(X);
        return 1;
    }
    XrCompilerInput compiler = {
        .analysis = {session, X, &machine->data_layout, NULL},
        .policy = {X->native_target_profile, false, NULL}
    };
    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    XrModuleStatus status = stdlib_module ? XR_MODULE_OK :
        xr_compile_module_identity_script_authority_from_source(xr_compile_state_resources(state),
            input, &authority, &authority_root);
    if (status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY)
        xr_compile_state_fail(state, status == XR_MODULE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    bool ok =
        status == XR_MODULE_OK &&
        (stdlib_module ? xr_compile_stdlib_to_file(&compiler, stdlib_module, input, output, flags)
                       : xr_compile_to_file(&compiler, input, output, flags, &authority));
    xr_compile_resources_free(authority_root);
    xray_vm_delete(X);
    return ok ? 0 : 1;
}
