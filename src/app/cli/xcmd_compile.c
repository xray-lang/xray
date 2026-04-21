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
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast_api.h"
#include "../../module/xbytecode_io.h"
#include "../../runtime/value/xchunk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "../../base/xmalloc.h"

static struct option compile_long_options[] = {
    {"help",         no_argument,       0, 'h'},
    {"output",       required_argument, 0, 'o'},
    {"format",       required_argument, 0, 'f'},
    {"strip-debug",  no_argument,       0, 's'},
    {"strip-source", no_argument,       0, 'S'},
    {"name",         required_argument, 0, 'n'},
    {0, 0, 0, 0}
};

void print_compile_help(void) {
    printf("Usage: xray compile [options] <file.xr>\n");
    printf("\n");
    printf("Compile source file to bytecode or C code\n");
    printf("\n");
    printf("Options:\n");
    printf("    -o, --output <file>  Output file path\n");
    printf("    -f, --format <fmt>   Output format: bytecode, c, header (default: infer from extension)\n");
    printf("    -s, --strip-debug    Remove debug info (line numbers, variable names)\n");
    printf("    -S, --strip-source   Remove source file path\n");
    printf("    -n, --name <name>    C variable name prefix (default: generated from filename)\n");
    printf("    -h, --help           Show this help\n");
    printf("\n");
    printf("Output formats:\n");
    printf("    .xrc     Bytecode file (binary)\n");
    printf("    .c       C source file (contains bytecode array)\n");
    printf("    .h       C header file (single array declaration)\n");
    printf("\n");
    printf("Examples:\n");
    printf("    xray compile app.xr -o app.xrc      # Compile to bytecode\n");
    printf("    xray compile app.xr -o app.c        # Compile to C source\n");
    printf("    xray compile app.xr -o app.c -s     # Compile and strip debug info\n");
    printf("    xray compile lib.xr -o lib.c -n mylib  # Specify variable name prefix\n");
    printf("\n");
}

// Generate C variable name from filename
static char* generate_var_name(const char *filename) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    // Remove extension
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);

    char *name = xr_malloc(len + 8);
    if (!name) return NULL;

    strcpy(name, "xr_bc_");
    size_t j = 6;

    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
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

int cmd_compile(int argc, char **argv) {
    const char *output_file = NULL;
    const char *var_name = NULL;
    XrOutputFormat explicit_format = XR_OUTPUT_AUTO;
    int flags = 0;

    // Reset getopt state
    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "ho:f:sSn:", compile_long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_compile_help();
                return 0;
            case 'o':
                output_file = optarg;
                break;
            case 'f':
                explicit_format = parse_format(optarg);
                if (explicit_format == XR_OUTPUT_AUTO) {
                    fprintf(stderr, "Error: unknown format '%s'\n", optarg);
                    return 1;
                }
                break;
            case 's':
                flags |= XR_BC_STRIP_DEBUG;
                break;
            case 'S':
                flags |= XR_BC_STRIP_SOURCE;
                break;
            case 'n':
                var_name = optarg;
                break;
            default:
                print_compile_help();
                return 1;
        }
    }

    // Check input file
    if (optind >= argc) {
        fprintf(stderr, "Error: missing input file\n");
        print_compile_help();
        return 1;
    }

    const char *input_file = argv[optind];

    // Default output file
    char default_output[512];
    if (!output_file) {
        const char *base = strrchr(input_file, '/');
        base = base ? base + 1 : input_file;
        const char *dot = strrchr(base, '.');
        size_t len = dot ? (size_t)(dot - base) : strlen(base);
        snprintf(default_output, sizeof(default_output), "%.*s.xrc", (int)len, base);
        output_file = default_output;
    }

    // Determine output format
    XrOutputFormat format = xr_detect_output_format(output_file, explicit_format);

    // Resources to clean up
    int result = 1;
    char *gen_var_name = NULL;
    XrayIsolate *X = NULL;
    char *source = NULL;
    AstNode *ast = NULL;

    // Generate variable name
    if (!var_name && (format == XR_OUTPUT_C_SOURCE || format == XR_OUTPUT_C_HEADER)) {
        gen_var_name = generate_var_name(input_file);
        var_name = gen_var_name;
    }

    // Create Isolate
    X = cli_create_isolate();
    if (!X) {
        fprintf(stderr, "Error: cannot create Isolate\n");
        goto cleanup;
    }

    // Read source file
    source = cli_read_file(input_file);
    if (!source) {
        fprintf(stderr, "Error: cannot open '%s'\n", input_file);
        goto cleanup;
    }

    // Parse
    ast = xr_parse_with_source(X, source, input_file);
    if (!ast) {
        fprintf(stderr, "Error: parsing failed\n");
        goto cleanup;
    }

    // Compile
    XrProto *proto = xr_compile_ast_with_source(X, ast, input_file);
    if (!proto) {
        fprintf(stderr, "Error: compilation failed\n");
        goto cleanup;
    }

    // Output
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
            fprintf(stderr, "Error: unknown output format\n");
            break;
    }

    if (!success) {
        fprintf(stderr, "Error: cannot write to '%s'\n", output_file);
    }

    result = success ? 0 : 1;

cleanup:
    if (ast) {
        xr_program_destroy(ast);
    }
    xr_free(source);
    if (X) xray_isolate_delete(X);
    xr_free(gen_var_name);
    return result;
}
