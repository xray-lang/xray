/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_compile.c - Source file to bytecode file compiler helper
 */

#include "xbytecode_io.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"
#include "xray_vm.h"
#include <stdio.h>

bool xr_compile_to_file(XrCompilerSession *session, const char *source_file,
                        const char *output_file, int flags) {
    if (!session) {
        xr_log_warning("compile", "compiler session is required");
        return false;
    }
    XrVMRuntime *X = xr_compiler_session_vm_host(session);
    if (!X) {
        xr_log_warning("compile", "compiler session has no VM host");
        return false;
    }

    // Read source file
    char *source = xr_file_read_all(source_file, "r", NULL);
    if (!source) {
        xr_log_warning("compile", "cannot open: %s", source_file);
        return false;
    }

    XrProto *proto = xr_compile_source_with_path(session, source, source_file);
    xr_free(source);

    if (!proto) {
        xr_log_warning("compile", "compilation failed: %s", source_file);
        return false;
    }

    // Serialize
    size_t bc_size;
    uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size);
    if (!bc) {
        xr_vm_proto_free(proto);
        xr_log_warning("compile", "serialization failed");
        return false;
    }

    xr_vm_proto_free(proto);

    // Write to file
    FILE *f = fopen(output_file, "wb");
    if (!f) {
        xr_free(bc);
        xr_log_warning("compile", "cannot create: %s", output_file);
        return false;
    }

    fwrite(bc, 1, bc_size, f);
    fclose(f);
    xr_free(bc);

    return true;
}
