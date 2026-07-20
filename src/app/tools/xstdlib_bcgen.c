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
 *   The final runtime embeds stdlib .xrc bytecode, but producing those .xrc
 *   files needs a compiler. This tiny build-only tool links against a bootstrap
 *   core that has embedded source plus an empty bytecode table, avoiding a
 *   final-runtime self-dependency cycle.
 */

#include "../../api/xisolate_profile.h"
#include "../../module/xbytecode_io.h"
#include "../../toolchain/xcompiler_session.h"
#include "xray_vm.h"
#include <stdio.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr, "usage: xray_stdlib_bcgen compile <input.xr> --output <output.xrc> "
                    "--format bytecode [--stdlib-module <canonical-name>] "
                    "[--strip-debug] [--strip-source]\n");
    return 2;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

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
            if (strcmp(fmt, "bytecode") != 0 && strcmp(fmt, "bc") != 0) {
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
            flags |= XR_BC_STRIP_DEBUG;
            continue;
        }
        if (strcmp(argv[i], "--strip-source") == 0) {
            flags |= XR_BC_STRIP_SOURCE;
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

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(X);
    bool ok = stdlib_module
                  ? xr_compile_stdlib_to_file(session, stdlib_module, input, output, flags)
                  : xr_compile_to_file(session, input, output, flags);
    xray_vm_delete(X);
    return ok ? 0 : 1;
}
