/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_native_writer.c - Real Program resource C generation
 */
#include "../execution/xr_typed_provider_fixture.h"
#include "aot/program/xr_backend_ir.h"

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "local") != 0 && strcmp(argv[2], "module") != 0)) return 2;
    XrValidatedProgram *program = typed_program_build(strcmp(argv[2], "module") == 0 ? 5u : 4u);
    XrTargetProfile *profile = typed_profile_build(0u);
    XrBackendIR *ir = NULL;
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic = {0};
    XrBackendOptions options = xr_backend_default_options();
    XrBackendStatus status = xr_backend_ir_build(program, profile, &options, &ir, &diagnostic);
    if (status == XR_BACKEND_OK) status = xr_backend_ir_emit_c(ir, false, &generated, &diagnostic);
    if (status != XR_BACKEND_OK) {
        fprintf(stderr, "typed native generation: status=%d operation=%u function=%u\n",
                status, diagnostic.operation_id, diagnostic.function_id);
    }
    bool written = false;
    if (status == XR_BACKEND_OK) {
        FILE *file = fopen(argv[1], "wb");
        if (file) {
            written = fwrite(generated.bytes, 1u, generated.size, file) == generated.size;
            written = fclose(file) == 0 && written;
        }
    }
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return written ? 0 : 5;
}
