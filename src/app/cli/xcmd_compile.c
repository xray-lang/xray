/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_compile.c - 'xray compile' command implementation
 *
 * KEY CONCEPT:
 *   Compiles source files to bytecode (.xrc) or C source (.c/.h).
 *   Output format is auto-detected from extension or specified via --format.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_isolate.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast_api.h"
#include "../../module/xbytecode_io.h"
#include "../../runtime/value/xchunk.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

// Generate C variable name from filename
static char *generate_var_name(const char *filename) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    // Remove extension
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t) (dot - base) : strlen(base);

    char *name = xr_malloc(len + 8);
    if (!name)
        return NULL;

    strcpy(name, "xr_bc_");
    size_t j = 6;

    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            name[j++] = c;
        } else {
            name[j++] = '_';
        }
    }
    name[j] = '\0';
    return name;
}

// Parse --format argument
static XrOutputFormat parse_format(const char *fmt) {
    if (strcmp(fmt, "bytecode") == 0 || strcmp(fmt, "bc") == 0) {
        return XR_OUTPUT_BYTECODE;
    } else if (strcmp(fmt, "c") == 0 || strcmp(fmt, "source") == 0) {
        return XR_OUTPUT_C_SOURCE;
    } else if (strcmp(fmt, "h") == 0 || strcmp(fmt, "header") == 0) {
        return XR_OUTPUT_C_HEADER;
    }
    return XR_OUTPUT_AUTO;
}

XR_FUNC int cmd_compile(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "compile expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *var_name = xr_cli_opt_string(&inv->options, "name", NULL);
    const char *fmt_str = xr_cli_opt_string(&inv->options, "format", NULL);

    int flags = 0;
    if (xr_cli_opt_present(&inv->options, "strip-debug"))
        flags |= XR_BC_STRIP_DEBUG;
    if (xr_cli_opt_present(&inv->options, "strip-source"))
        flags |= XR_BC_STRIP_SOURCE;

    /* Parse explicit format */
    XrOutputFormat explicit_format = XR_OUTPUT_AUTO;
    if (fmt_str) {
        explicit_format = parse_format(fmt_str);
        if (explicit_format == XR_OUTPUT_AUTO) {
            xr_cli_error("compile", "unknown format '%s'", fmt_str);
            return XR_CLI_EXIT_USAGE;
        }
    }

    /* Default output file */
    char default_output[512];
    if (!output_file) {
        const char *base = strrchr(input_file, '/');
        base = base ? base + 1 : input_file;
        const char *dot = strrchr(base, '.');
        size_t len = dot ? (size_t) (dot - base) : strlen(base);
        snprintf(default_output, sizeof(default_output), "%.*s.xrc", (int) len, base);
        output_file = default_output;
    }

    /* Determine output format */
    XrOutputFormat format = xr_detect_output_format(output_file, explicit_format);

    /* Resources to clean up */
    int result = XR_CLI_EXIT_FAIL;
    char *gen_var_name = NULL;
    XrayIsolate *X = NULL;
    char *source = NULL;
    AstNode *ast = NULL;

    /* Generate variable name */
    if (!var_name && (format == XR_OUTPUT_C_SOURCE || format == XR_OUTPUT_C_HEADER)) {
        gen_var_name = generate_var_name(input_file);
        var_name = gen_var_name;
    }

    /* Create isolate */
    X = xr_cli_isolate_new(XR_CLI_ISOLATE_RUN);
    if (!X) {
        xr_cli_error("compile", "failed to create isolate");
        result = XR_CLI_EXIT_INTERNAL;
        goto cleanup;
    }

    /* Read source file */
    source = xr_cli_read_file(input_file);
    if (!source) {
        xr_cli_error("compile", "cannot open '%s'", input_file);
        goto cleanup;
    }

    /* Parse */
    ast = xr_parse_with_source(X, source, input_file);
    if (!ast) {
        xr_cli_error("compile", "parsing failed");
        goto cleanup;
    }

    /* Compile */
    XrProto *proto = xr_compile_ast_with_source(X, ast, input_file);
    if (!proto) {
        xr_cli_error("compile", "compilation failed");
        goto cleanup;
    }

    /* Output */
    bool success = false;

    switch (format) {
        case XR_OUTPUT_BYTECODE: {
            size_t bc_size;
            uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size);
            if (bc) {
                FILE *out = fopen(output_file, "wb");
                if (out) {
                    fwrite(bc, 1, bc_size, out);
                    fclose(out);
                    success = true;
                    printf("Compiled: %s (%zu bytes)\n", output_file, bc_size);
                }
                xr_free(bc);
            }
            break;
        }

        case XR_OUTPUT_C_SOURCE:
        case XR_OUTPUT_C_HEADER:
            success = xr_output_c_source(X, proto, output_file, var_name, flags);
            if (success) {
                printf("Compiled: %s\n", output_file);
            }
            break;

        default:
            xr_cli_error("compile", "unknown output format");
            break;
    }

    if (!success) {
        xr_cli_error("compile", "cannot write to '%s'", output_file);
    }

    result = success ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;

cleanup:
    if (ast) {
        xr_program_destroy(ast);
    }
    xr_free(source);
    if (X)
        xray_isolate_delete(X);
    xr_free(gen_var_name);
    return result;
}
