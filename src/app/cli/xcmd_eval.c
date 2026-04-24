/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_eval.c - Explicit eval command
 *
 * KEY CONCEPT:
 *   `xray eval '<code>'` evaluates a code string directly.
 *   `xray -e '<code>'` is a dispatch shortcut that routes here.
 *   Supports stdin reading with `xray eval -` (reads from stdin).
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../vm/xvm_internal.h"
#include <stdio.h>
#include <string.h>

/* New-style handler: receives a fully parsed XrCliInvocation.
 * The unified parser guarantees exactly 1 positional (the code string). */
XR_FUNC int cmd_eval(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "eval expects exactly 1 positional");

    const char *code = inv->positionals[0];

    /* "-" means read from stdin */
    char *stdin_code = NULL;
    if (strcmp(code, "-") == 0) {
        stdin_code = xr_cli_read_stdin();
        if (!stdin_code || stdin_code[0] == '\0') {
            xr_cli_error("eval", "no input from stdin");
            xr_free(stdin_code);
            return XR_CLI_EXIT_FAIL;
        }
        code = stdin_code;
    }

    /* Create isolate and execute */
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);

    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) {
        xr_cli_error("eval", "failed to create isolate");
        xr_free(stdin_code);
        return XR_CLI_EXIT_INTERNAL;
    }
    xr_multicore_init(iso, 0);

    int result = xray_isolate_dostring(iso, code);

    xr_multicore_destroy(iso);
    xray_isolate_delete(iso);
    xr_free(stdin_code);

    return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
