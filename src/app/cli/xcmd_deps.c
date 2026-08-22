/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_deps.c - 'xray deps' command implementation
 *
 * KEY CONCEPT:
 *   Analyzes project dependencies and generates install scripts.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "../../api/xisolate_profile.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../module/xbundle.h"
#include "../../module/xmodule_identity.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// Write a JSON-escaped string to file (handles \, ", and control chars)
static void fprint_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':
                fputs("\\\"", f);
                break;
            case '\\':
                fputs("\\\\", f);
                break;
            case '\n':
                fputs("\\n", f);
                break;
            case '\t':
                fputs("\\t", f);
                break;
            default:
                fputc(*s, f);
                break;
        }
    }
    fputc('"', f);
}

typedef enum {
    OUTPUT_SHELL,
    OUTPUT_JSON,
    OUTPUT_LIST
} OutputFormat;

static int bundle_count_kind(const XrBundle *bundle, XrModuleKind kind, bool exclude_entry) {
    int count = 0;
    for (int i = 0; i < bundle->count; i++) {
        if (bundle->entries[i].kind == kind && (!exclude_entry || i != bundle->entry_index))
            count++;
    }
    return count;
}

XR_FUNC int cmd_deps(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "deps expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);

    OutputFormat format = OUTPUT_SHELL;
    if (xr_cli_opt_present(&inv->options, "json"))
        format = OUTPUT_JSON;
    else if (xr_cli_opt_present(&inv->options, "list"))
        format = OUTPUT_LIST;
    else if (xr_cli_opt_present(&inv->options, "shell"))
        format = OUTPUT_SHELL;

    /* Create isolate and analyze dependencies */
    XrVMRuntime *X = xr_isolate_profile_new(XR_ISOLATE_PROFILE_RUN);
    if (!X) {
        xr_cli_error("deps", "failed to create isolate");
        return XR_CLI_EXIT_INTERNAL;
    }

    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    XrBundle *bundle =
        xr_module_identity_script_authority_from_source(input_file, &authority, &authority_root)
            ? xr_bundle_create_ex(X, input_file, &authority, XR_BUNDLE_DEFAULT)
            : NULL;
    xr_free(authority_root);
    xray_vm_delete(X);

    if (!bundle) {
        xr_cli_error("deps", "dependency analysis failed for '%s'", input_file);
        return XR_CLI_EXIT_FAIL;
    }

    /* Open output file */
    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) {
            xr_cli_error("deps", "cannot create '%s'", output_file);
            xr_bundle_free(bundle);
            return XR_CLI_EXIT_FAIL;
        }
    }

    /* Write output */
    switch (format) {
        case OUTPUT_LIST:
            if (bundle_count_kind(bundle, XR_MOD_STDLIB, false) > 0) {
                fprintf(out, "# Stdlib\n");
                for (int i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_MOD_STDLIB)
                        fprintf(out, "%s\n", bundle->entries[i].path);
            }
            if (bundle_count_kind(bundle, XR_MOD_PACKAGE, false) > 0) {
                fprintf(out, "# Third-party packages\n");
                for (int i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_MOD_PACKAGE)
                        fprintf(out, "%s\n", bundle->entries[i].path);
            }
            if (bundle_count_kind(bundle, XR_MOD_FILE, true) > 0) {
                fprintf(out, "# Local modules\n");
                for (int i = 0; i < bundle->count; i++)
                    if (i != bundle->entry_index && bundle->entries[i].kind == XR_MOD_FILE)
                        fprintf(out, "%s\n", bundle->entries[i].path);
            }
            break;

        case OUTPUT_JSON:
            fprintf(out, "{\n");
            fprintf(out, "  \"entry\": ");
            fprint_json_string(out, bundle->entry_path);
            fprintf(out, ",\n");

            fprintf(out, "  \"stdlib\": [");
            int written = 0;
            for (int i = 0; i < bundle->count; i++) {
                if (bundle->entries[i].kind != XR_MOD_STDLIB)
                    continue;
                if (written++ > 0)
                    fprintf(out, ", ");
                fprint_json_string(out, bundle->entries[i].path);
            }
            fprintf(out, "],\n");

            fprintf(out, "  \"packages\": [");
            written = 0;
            for (int i = 0; i < bundle->count; i++) {
                if (bundle->entries[i].kind != XR_MOD_PACKAGE)
                    continue;
                if (written++ > 0)
                    fprintf(out, ", ");
                fprint_json_string(out, bundle->entries[i].path);
            }
            fprintf(out, "],\n");

            fprintf(out, "  \"local_modules\": [");
            written = 0;
            for (int i = 0; i < bundle->count; i++) {
                if (i == bundle->entry_index || bundle->entries[i].kind != XR_MOD_FILE)
                    continue;
                if (written++ > 0)
                    fprintf(out, ", ");
                fprint_json_string(out, bundle->entries[i].path);
            }
            fprintf(out, "]\n");
            fprintf(out, "}\n");
            break;

        case OUTPUT_SHELL:
        default:
            fprintf(out, "#!/bin/bash\n");
            fprintf(out, "# Dependency install script\n");
            fprintf(out, "# Auto-generated by xray deps\n");
            fprintf(out, "# Entry: %s\n", bundle->entry_path);
            fprintf(out, "\n");
            fprintf(out, "set -e\n");
            fprintf(out, "\n");

            if (bundle_count_kind(bundle, XR_MOD_PACKAGE, false) > 0) {
                fprintf(out, "echo \"Installing third-party package dependencies...\"\n");
                for (int i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_MOD_PACKAGE)
                        fprintf(out, "xray pkg add %s\n", bundle->entries[i].path);
                fprintf(out, "\n");
                fprintf(out, "echo \"All dependencies installed\"\n");
            } else {
                fprintf(out, "echo \"No third-party package dependencies\"\n");
            }

            if (bundle_count_kind(bundle, XR_MOD_STDLIB, false) > 0) {
                fprintf(out, "\n");
                fprintf(out, "# Stdlib dependencies (built-in, no install needed):\n");
                for (int i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_MOD_STDLIB)
                        fprintf(out, "#   - %s\n", bundle->entries[i].path);
            }
            break;
    }

    if (output_file) {
        fclose(out);
        printf("Dependency script generated: %s\n", output_file);
        if (format == OUTPUT_SHELL) {
            printf("Run with: bash %s\n", output_file);
        }
    }

    xr_bundle_free(bundle);
    return 0;
}
