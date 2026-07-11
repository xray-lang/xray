/*
 * bytecode_embed_runner.c - Minimal embedded bytecode VM smoke.
 *
 * This target intentionally links xray_vm_runtime, not xray_core. It must be
 * able to execute precompiled bytecode without pulling parser/analyzer/compiler
 * or build-toolchain objects into the final binary.
 */

#include "module/xbytecode_io.h"
#include "module/xmodule.h"
#include "xray_vm.h"
#include <stdio.h>
#include <string.h>

static int run_bytecode_file_expect_line(const char *bytecode_path, const char *expected_line) {
    XrVMConfig params;
    xray_vm_config_init(&params);

    XrVMRuntime *iso = xray_vm_new_runtime(&params);
    if (!iso)
        return 2;
    xr_module_system_init(iso);

    FILE *out = tmpfile();
    if (!out) {
        xr_module_system_free(iso);
        xray_vm_delete(iso);
        return 3;
    }
    xray_vm_set_stdout(iso, out);

    int rc = xr_run_bytecode_file(iso, bytecode_path);

    fflush(out);
    rewind(out);
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, out);
    fclose(out);
    xr_module_system_free(iso);
    xray_vm_delete(iso);

    if (rc != 0)
        return 4;

    size_t expected_len = strlen(expected_line);
    if (n != expected_len + 1 || strncmp(buf, expected_line, expected_len) != 0 ||
        buf[expected_len] != '\n' || buf[expected_len + 1] != '\0') {
        fprintf(stderr, "unexpected bytecode output: '%s', expected '%s\\n'\n", buf, expected_line);
        return 5;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3)
        return run_bytecode_file_expect_line(argv[1], argv[2]);
    fprintf(stderr, "usage: %s <bytecode.xrc> <expected-line>\n", argv[0]);
    return 6;
}
