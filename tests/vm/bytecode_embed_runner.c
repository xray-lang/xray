/*
 * bytecode_embed_runner.c - Minimal embedded bytecode VM smoke.
 *
 * This target intentionally links xray_vm_runtime, not xray_core. It must be
 * able to execute precompiled bytecode without pulling parser/analyzer/compiler
 * or build-toolchain objects into the final binary.
 */

#include "module/xbytecode_io.h"
#include "xray_isolate.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Generated from `print(7)` with stripped debug/source using XR_BC_VERSION 7.
static const uint32_t xr_embed_smoke_size = 97;
static const uint8_t xr_embed_smoke[97] = {
    0x58, 0x52, 0x41, 0x59, 0x07, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x3c, 0x6d, 0x61, 0x69,
    0x6e, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x06, 0x00, 0x00, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x3f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int main(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_runtime(&params);

    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) {
        return 2;
    }

    FILE *out = tmpfile();
    if (!out) {
        xray_isolate_delete(iso);
        return 3;
    }
    xray_isolate_set_stdout(iso, out);

    int rc = xr_eval_bytecode(iso, xr_embed_smoke, xr_embed_smoke_size);

    fflush(out);
    rewind(out);
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, out);
    fclose(out);
    xray_isolate_delete(iso);

    if (rc != 0) {
        return 4;
    }
    if (n < 2 || strcmp(buf, "7\n") != 0) {
        fprintf(stderr, "unexpected bytecode output: '%s'\n", buf);
        return 5;
    }
    return 0;
}
