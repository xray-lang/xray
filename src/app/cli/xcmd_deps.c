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
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../module/xbundle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>

// Write a JSON-escaped string to file (handles \, ", and control chars)
static void fprint_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:   fputc(*s, f);     break;
        }
    }
    fputc('"', f);
}

void print_deps_help(void) {
    printf("Usage: xray deps [options] <file.xr>\n");
    printf("\n");
    printf("Analyze project dependencies and generate install scripts\n");
    printf("\n");
    printf("Options:\n");
    printf("    -o, --output <file>  Output to file (default: stdout)\n");
    printf("    --shell              Generate shell script (default)\n");
    printf("    --json               Output JSON format\n");
    printf("    --list               List dependencies only\n");
    printf("    -h, --help           Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("    xray deps app.xr                      # Show dependencies\n");
    printf("    xray deps app.xr -o install.sh        # Generate install script\n");
    printf("    xray deps app.xr --json               # JSON format output\n");
    printf("\n");
}

typedef enum {
    OUTPUT_SHELL,
    OUTPUT_JSON,
    OUTPUT_LIST
} OutputFormat;

int cmd_deps(int argc, char **argv) {
    static struct option long_options[] = {
        {"output", required_argument, 0, 'o'},
        {"shell", no_argument, 0, 's'},
        {"json", no_argument, 0, 'j'},
        {"list", no_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    const char *output_file = NULL;
    const char *input_file = NULL;
    OutputFormat format = OUTPUT_SHELL;
    
    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "o:sjlh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'o':
                output_file = optarg;
                break;
            case 's':
                format = OUTPUT_SHELL;
                break;
            case 'j':
                format = OUTPUT_JSON;
                break;
            case 'l':
                format = OUTPUT_LIST;
                break;
            case 'h':
                print_deps_help();
                return 0;
            default:
                return 1;
        }
    }
    
    if (optind < argc) {
        input_file = argv[optind];
    }
    
    if (input_file == NULL) {
        fprintf(stderr, "Error: no input file specified\n");
        print_deps_help();
        return 1;
    }
    
    // Create Isolate and analyze dependencies
    XrayIsolate *X = cli_create_isolate();
    if (!X) {
        fprintf(stderr, "Error: cannot create Isolate\n");
        return 1;
    }
    
    XrBundle *bundle = xr_bundle_create(X, input_file);
    xray_isolate_delete(X);
    
    if (!bundle) {
        fprintf(stderr, "Error: dependency analysis failed\n");
        return 1;
    }
    
    // Open output file
    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) {
            fprintf(stderr, "Error: cannot create '%s'\n", output_file);
            xr_bundle_free(bundle);
            return 1;
        }
    }
    
    // Output dependency info
    switch (format) {
        case OUTPUT_LIST:
            if (bundle->stdlib.count > 0) {
                fprintf(out, "# Stdlib\n");
                for (int i = 0; i < bundle->stdlib.count; i++) {
                    fprintf(out, "%s\n", bundle->stdlib.deps[i]);
                }
            }
            if (bundle->packages.count > 0) {
                fprintf(out, "# Third-party packages\n");
                for (int i = 0; i < bundle->packages.count; i++) {
                    fprintf(out, "%s\n", bundle->packages.deps[i]);
                }
            }
            if (bundle->count > 1) {
                fprintf(out, "# Local modules\n");
                for (int i = 0; i < bundle->count - 1; i++) {
                    fprintf(out, "%s\n", bundle->entries[i].path);
                }
            }
            break;
            
        case OUTPUT_JSON:
            fprintf(out, "{\n");
            fprintf(out, "  \"entry\": ");
            fprint_json_string(out, bundle->entry_path);
            fprintf(out, ",\n");
            
            fprintf(out, "  \"stdlib\": [");
            for (int i = 0; i < bundle->stdlib.count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprint_json_string(out, bundle->stdlib.deps[i]);
            }
            fprintf(out, "],\n");
            
            fprintf(out, "  \"packages\": [");
            for (int i = 0; i < bundle->packages.count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprint_json_string(out, bundle->packages.deps[i]);
            }
            fprintf(out, "],\n");
            
            fprintf(out, "  \"local_modules\": [");
            for (int i = 0; i < bundle->count - 1; i++) {
                if (i > 0) fprintf(out, ", ");
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
            
            if (bundle->packages.count > 0) {
                fprintf(out, "echo \"Installing third-party package dependencies...\"\n");
                for (int i = 0; i < bundle->packages.count; i++) {
                    fprintf(out, "xray pkg add %s\n", bundle->packages.deps[i]);
                }
                fprintf(out, "\n");
                fprintf(out, "echo \"All dependencies installed\"\n");
            } else {
                fprintf(out, "echo \"No third-party package dependencies\"\n");
            }
            
            if (bundle->stdlib.count > 0) {
                fprintf(out, "\n");
                fprintf(out, "# Stdlib dependencies (built-in, no install needed):\n");
                for (int i = 0; i < bundle->stdlib.count; i++) {
                    fprintf(out, "#   - %s\n", bundle->stdlib.deps[i]);
                }
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
