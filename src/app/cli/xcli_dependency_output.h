/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_dependency_output.h - Three dependency report formats from owned Source facts
 */
#ifndef XCLI_DEPENDENCY_OUTPUT_H
#define XCLI_DEPENDENCY_OUTPUT_H
#include "../../xir/xxir_source_dependencies.h"
#include <stdio.h>
#include <stdbool.h>
/* Preserve UTF-8 bytes while escaping every JSON control byte. */
static void fprint_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
}

/* Single quotes preserve each byte as one shell argument, including newlines. */
static void deps_shell_argument(FILE *f, const char *s) {
    fputc('\'', f);
    for (; *s; ++s) {
        if (*s == '\'') fputs("'\\''", f);
        else fputc((unsigned char)*s, f);
    }
    fputc('\'', f);
}

/* Control bytes must not terminate a comment and introduce shell commands. */
static void deps_shell_comment(FILE *f, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) fprintf(f, "\\x%02x", (unsigned)*p);
        else fputc(*p, f);
    }
}

typedef enum {
    OUTPUT_SHELL,
    OUTPUT_JSON,
    OUTPUT_LIST
} OutputFormat;

static int bundle_count_kind(const XrXirSourceDependencies *bundle, XrXirSourceDependencyKind kind, bool exclude_entry) {
    int count = 0;
    for (uint32_t i = 0; i < bundle->count; i++) {
        if (bundle->entries[i].kind == kind && (!exclude_entry || i != bundle->entry))
            count++;
    }
    return count;
}

static bool deps_emit(FILE *out, const XrXirSourceDependencies *bundle, OutputFormat format) {
    if (!out || !bundle || (unsigned)format > OUTPUT_LIST) return false;
    /* Write output */
    switch (format) {
        case OUTPUT_LIST:
            if (bundle_count_kind(bundle, XR_XIR_DEPENDENCY_STDLIB, false) > 0) {
                fprintf(out, "# Stdlib\n");
                for (uint32_t i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_XIR_DEPENDENCY_STDLIB)
                        fprintf(out, "%s\n", bundle->entries[i].path);
            }
            if (bundle_count_kind(bundle, XR_XIR_DEPENDENCY_PACKAGE, false) > 0) {
                fprintf(out, "# Third-party packages\n");
                for (uint32_t i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_XIR_DEPENDENCY_PACKAGE)
                        fprintf(out, "%s\n", bundle->entries[i].path);
            }
            if (bundle_count_kind(bundle, XR_XIR_DEPENDENCY_FILE, true) > 0) {
                fprintf(out, "# Local modules\n");
                for (uint32_t i = 0; i < bundle->count; i++)
                    if (i != bundle->entry && bundle->entries[i].kind == XR_XIR_DEPENDENCY_FILE)
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
            for (uint32_t i = 0; i < bundle->count; i++) {
                if (bundle->entries[i].kind != XR_XIR_DEPENDENCY_STDLIB)
                    continue;
                if (written++ > 0)
                    fprintf(out, ", ");
                fprint_json_string(out, bundle->entries[i].path);
            }
            fprintf(out, "],\n");

            fprintf(out, "  \"packages\": [");
            written = 0;
            for (uint32_t i = 0; i < bundle->count; i++) {
                if (bundle->entries[i].kind != XR_XIR_DEPENDENCY_PACKAGE)
                    continue;
                if (written++ > 0)
                    fprintf(out, ", ");
                fprint_json_string(out, bundle->entries[i].path);
            }
            fprintf(out, "],\n");

            fprintf(out, "  \"local_modules\": [");
            written = 0;
            for (uint32_t i = 0; i < bundle->count; i++) {
                if (i == bundle->entry || bundle->entries[i].kind != XR_XIR_DEPENDENCY_FILE)
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
            fputs("# Entry: ", out);
            deps_shell_comment(out, bundle->entry_path);
            fputc('\n', out);
            fprintf(out, "\n");
            fprintf(out, "set -e\n");
            fprintf(out, "\n");

            if (bundle_count_kind(bundle, XR_XIR_DEPENDENCY_PACKAGE, false) > 0) {
                fprintf(out, "echo \"Installing third-party package dependencies...\"\n");
                for (uint32_t i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_XIR_DEPENDENCY_PACKAGE) {
                        fputs("xray pkg add ", out);
                        deps_shell_argument(out, bundle->entries[i].path);
                        fputc('\n', out);
                    }
                fprintf(out, "\n");
                fprintf(out, "echo \"All dependencies installed\"\n");
            } else {
                fprintf(out, "echo \"No third-party package dependencies\"\n");
            }

            if (bundle_count_kind(bundle, XR_XIR_DEPENDENCY_STDLIB, false) > 0) {
                fprintf(out, "\n");
                fprintf(out, "# Stdlib dependencies (built-in, no install needed):\n");
                for (uint32_t i = 0; i < bundle->count; i++)
                    if (bundle->entries[i].kind == XR_XIR_DEPENDENCY_STDLIB) {
                        fputs("#   - ", out);
                        deps_shell_comment(out, bundle->entries[i].path);
                        fputc('\n', out);
                    }
            }
            break;
    }

    return ferror(out) == 0;
}
#endif // XCLI_DEPENDENCY_OUTPUT_H
