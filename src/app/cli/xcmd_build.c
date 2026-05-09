/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build.c - 'xray build' command implementation
 *
 * KEY CONCEPT:
 *   Two build modes:
 *   1. Default: bytecode embedding (full xray features, needs runtime)
 *   2. --native: AOT + bytecode hybrid, links xray runtime (like Go)
 *
 * WHY THIS DESIGN:
 *   - Bytecode mode supports all xray features (coroutines, dynamic types)
 *   - Native mode combines AOT performance with full runtime support
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_isolate.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../module/xbundle.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"

/* ========== Shared Helpers ========== */

// Invoke C compiler to link generated C source (+ optional .o) into executable
static int invoke_cc(const char *cc, const char *opt_flag, const char *output_file,
                     const char *c_file, const char *obj_file, bool strip_symbols,
                     const char *sysroot) {
    const char *xray_include = getenv("XRAY_INCLUDE");
    const char *xray_lib = getenv("XRAY_LIB");

    char include_path[512], lib_path[512];
    if (sysroot) {
        snprintf(include_path, sizeof(include_path), "%s/include/xray", sysroot);
        snprintf(lib_path, sizeof(lib_path), "%s/lib", sysroot);
        xray_include = include_path;
        xray_lib = lib_path;
    } else {
        if (!xray_include)
            xray_include = "/usr/local/include/xray";
        if (!xray_lib)
            xray_lib = "/usr/local/lib";
    }

    char include_flag[600], lib_flag[600];
    snprintf(include_flag, sizeof(include_flag), "-I%s", xray_include);
    snprintf(lib_flag, sizeof(lib_flag), "-L%s", xray_lib);

    const char *spawn_argv[26];
    int ai = 0;
    spawn_argv[ai++] = cc;
    spawn_argv[ai++] = opt_flag;
    spawn_argv[ai++] = "-o";
    spawn_argv[ai++] = output_file;
    spawn_argv[ai++] = c_file;
    if (obj_file)
        spawn_argv[ai++] = obj_file;
    spawn_argv[ai++] = include_flag;
#ifdef XRT_AOT_INCLUDE_DIR
    spawn_argv[ai++] = "-I" XRT_AOT_INCLUDE_DIR;
#endif
    spawn_argv[ai++] = lib_flag;
    spawn_argv[ai++] = "-lxray_core";
    spawn_argv[ai++] = "-lpthread";
    spawn_argv[ai++] = "-lm";
#ifdef XR_OS_MACOS
    spawn_argv[ai++] = "-Wl,-dead_strip";
#else
    spawn_argv[ai++] = "-ffunction-sections";
    spawn_argv[ai++] = "-fdata-sections";
    spawn_argv[ai++] = "-Wl,--gc-sections";
#endif
    if (strip_symbols)
        spawn_argv[ai++] = "-Wl,-x";
    spawn_argv[ai] = NULL;

    printf("Linking:");
    for (int i = 0; spawn_argv[i]; i++)
        printf(" %s", spawn_argv[i]);
    printf("\n");

    XrProcId pid = xr_proc_spawn(cc, spawn_argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start compiler '%s'\n", cc);
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: linking failed\n");
        fprintf(stderr, "Tip: set XRAY_INCLUDE and XRAY_LIB environment variables\n");
        fprintf(stderr, "  export XRAY_INCLUDE=/path/to/xray/include\n");
        fprintf(stderr, "  export XRAY_LIB=/path/to/xray/build\n");
        return 1;
    }
    return 0;
}

// Invoke C compiler for standalone AOT binary (no libxray_core)
static int invoke_cc_standalone(const char *cc, const char *opt_flag, const char *output_file,
                                const char *c_file, bool strip_symbols, const char *sysroot) {
    char aot_include[600] = "";
#ifdef XRT_AOT_INCLUDE_DIR
    snprintf(aot_include, sizeof(aot_include), "-I" XRT_AOT_INCLUDE_DIR);
#else
    const char *xray_include = getenv("XRAY_INCLUDE");
    if (sysroot) {
        snprintf(aot_include, sizeof(aot_include), "-I%s/include/xray", sysroot);
    } else if (xray_include) {
        snprintf(aot_include, sizeof(aot_include), "-I%s", xray_include);
    } else {
        snprintf(aot_include, sizeof(aot_include), "-I/usr/local/include/xray");
    }
#endif

    const char *spawn_argv[20];
    int ai = 0;
    spawn_argv[ai++] = cc;
    spawn_argv[ai++] = opt_flag;
    spawn_argv[ai++] = "-o";
    spawn_argv[ai++] = output_file;
    spawn_argv[ai++] = c_file;
    if (aot_include[0])
        spawn_argv[ai++] = aot_include;
    spawn_argv[ai++] = "-lm";
#ifdef XR_OS_MACOS
    spawn_argv[ai++] = "-Wl,-dead_strip";
#else
    spawn_argv[ai++] = "-ffunction-sections";
    spawn_argv[ai++] = "-fdata-sections";
    spawn_argv[ai++] = "-Wl,--gc-sections";
#endif
    if (strip_symbols)
        spawn_argv[ai++] = "-Wl,-x";
    spawn_argv[ai] = NULL;

    printf("Linking (standalone):");
    for (int i = 0; spawn_argv[i]; i++)
        printf(" %s", spawn_argv[i]);
    printf("\n");

    XrProcId pid = xr_proc_spawn(cc, spawn_argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start compiler '%s'\n", cc);
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: standalone linking failed\n");
        return 1;
    }
    return 0;
}

// Write the main() function for bytecode execution into a C file
// bundle_source is the output of xr_bundle_to_c_source()
static void write_bytecode_main(FILE *f, const char *bundle_source) {
    // Headers (bundle source uses strcmp but doesn't include string.h)
    fprintf(f, "#include <stdio.h>\n"
               "#include <stdlib.h>\n"
               "#include <string.h>\n"
               "#include <stdint.h>\n"
               "#include <stddef.h>\n\n");

    // Bundle-generated data (module bytecode arrays, module table, lookup function)
    fprintf(f, "%s\n\n", bundle_source);

    // Main: use XR_INIT_RUNTIME for minimal footprint (no compiler/analyzer/stdlib)
    fprintf(f, "#include \"xray_isolate.h\"\n"
               "extern int xr_eval_bytecode(void*, const uint8_t*, size_t);\n"
               "extern void xr_multicore_init(void*, int);\n"
               "extern void xr_multicore_destroy(void*);\n"
               "\n"
               "int main(int argc, char **argv) {\n"
               "    XrayIsolateParams params;\n"
               "    xray_isolate_params_init(&params);\n"
               "    params.init_flags = XR_INIT_RUNTIME;\n"
               "    params.script_argc = argc > 1 ? argc - 1 : 0;\n"
               "    params.script_argv = argc > 1 ? argv + 1 : NULL;\n"
               "    XrayIsolate *X = xray_isolate_new(&params);\n"
               "    if (!X) { fprintf(stderr, \"Failed to create runtime\\n\"); return 1; }\n"
               "    xr_multicore_init(X, 0);\n"
               "    const XrEmbeddedModule *entry = &xr_app_modules[xr_app_entry_index];\n"
               "    int result = xr_eval_bytecode(X, entry->bc, entry->size);\n"
               "    xr_multicore_destroy(X);\n"
               "    xray_isolate_delete(X);\n"
               "    return result;\n"
               "}\n");
}

/* ========== Optimization Flag ========== */

static const char *make_opt_flag(const char *level) {
    if (!level)
        return "-O2";
    if (strcmp(level, "0") == 0)
        return "-O0";
    if (strcmp(level, "1") == 0)
        return "-O1";
    if (strcmp(level, "2") == 0)
        return "-O2";
    if (strcmp(level, "3") == 0)
        return "-O3";
    if (strcmp(level, "s") == 0)
        return "-Os";
    if (strcmp(level, "fast") == 0)
        return "-Ofast";
    fprintf(stderr, "Warning: unknown optimization level '%s', using -O2\n", level);
    return "-O2";
}

/* ========== Build Sub-Modes (forward declarations) ========== */

static int cmd_build_bytecode(const char *input, const char *output, const char *cc,
                              const char *opt_flag, bool c_only, bool strip, const char *sysroot);
static int cmd_build_native(const char *input, const char *output, const char *cc,
                            const char *opt_flag, bool c_only, bool strip, const char *sysroot);

/* ========== CLI Entry Point ========== */

XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "build expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *cc = xr_cli_opt_string(&inv->options, "cc", "cc");
    const char *opt_level = xr_cli_opt_string(&inv->options, "opt", NULL);
    const char *sysroot = xr_cli_opt_string(&inv->options, "sysroot", NULL);
    bool c_only = xr_cli_opt_bool(&inv->options, "c-only");
    bool strip_symbols = xr_cli_opt_bool(&inv->options, "strip");
    bool native_mode = xr_cli_opt_bool(&inv->options, "native");

    if (!output_file)
        output_file = c_only ? "app.c" : "a.out";

    const char *opt_flag = make_opt_flag(opt_level);

    if (native_mode) {
        return cmd_build_native(input_file, output_file, cc, opt_flag, c_only, strip_symbols,
                                sysroot);
    }
    return cmd_build_bytecode(input_file, output_file, cc, opt_flag, c_only, strip_symbols,
                              sysroot);
}

/* ========== Bytecode Bundling (default mode) ========== */

static int cmd_build_bytecode(const char *input, const char *output, const char *cc,
                              const char *opt_flag, bool c_only, bool strip, const char *sysroot) {
    printf("[bytecode] Building: %s\n", input);

    XrayIsolate *X = xr_cli_isolate_new(XR_CLI_ISOLATE_RUN);
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }

    XrBundle *bundle = xr_bundle_create_ex(X, input, XR_BUNDLE_DEFAULT);
    if (!bundle) {
        fprintf(stderr, "Error: bytecode bundling failed\n");
        xray_isolate_delete(X);
        return 1;
    }
    xray_isolate_delete(X);

    printf("Modules: %d\n", bundle->count);
    for (int i = 0; i < bundle->count; i++)
        printf("  %s (%zu bytes)\n", bundle->entries[i].path, bundle->entries[i].bc_size);

    char *bc_source = xr_bundle_to_c_source(bundle, "xr_app");
    xr_bundle_free(bundle);
    if (!bc_source) {
        fprintf(stderr, "Error: C source generation failed\n");
        return 1;
    }

    char c_file[512];
    if (c_only)
        snprintf(c_file, sizeof(c_file), "%s", output);
    else
        snprintf(c_file, sizeof(c_file), "/tmp/xray_bc_%d.c", (int) xr_proc_self_pid());

    FILE *f = fopen(c_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", c_file);
        xr_free(bc_source);
        return 1;
    }
    write_bytecode_main(f, bc_source);
    fclose(f);
    xr_free(bc_source);

    if (c_only) {
        printf("Generated: %s\n", output);
        return 0;
    }

    int ret = invoke_cc(cc, opt_flag, output, c_file, NULL, strip, sysroot);
    xr_fs_remove(c_file);
    if (ret == 0)
        printf("Generated: %s\n", output);
    return ret;
}

/* ========== Native Build (--native, Xi IR AOT pipeline) ========== */

#include "../../aot/xaot_driver.h"

static int cmd_build_native(const char *input, const char *output, const char *cc,
                            const char *opt_flag, bool c_only, bool strip, const char *sysroot) {
    XaotBuildResult aot_result;
    int rc = xaot_build(input, &aot_result);
    if (rc != 0)
        return rc;

    /* Write generated C to file */
    char c_file[512];
    if (c_only)
        snprintf(c_file, sizeof(c_file), "%s", output);
    else
        snprintf(c_file, sizeof(c_file), "/tmp/xray_xi_%d.c", (int) xr_proc_self_pid());

    FILE *f = fopen(c_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", c_file);
        xr_free(aot_result.c_source);
        return 1;
    }
    fprintf(f, "%s", aot_result.c_source);
    fclose(f);
    xr_free(aot_result.c_source);

    if (c_only) {
        printf("Generated: %s\n", output);
        return 0;
    }

    int ret = invoke_cc_standalone(cc, opt_flag, output, c_file, strip, sysroot);
    xr_fs_remove(c_file);
    if (ret == 0)
        printf("Generated: %s\n", output);
    return ret;
}
