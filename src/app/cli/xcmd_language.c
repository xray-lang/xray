/*
 * xray - Lightweight typed scripting with native concurrency
 * xcmd_language.c - Public language-surface inventory
 */

#include "xcli_spec.h"
#include "xcli_diag.h"
#include "../../frontend/parser/xattribute_registry.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

static void print_json_string(const char *value) {
    const unsigned char *p = (const unsigned char *) (value ? value : "");
    putchar('"');
    while (*p) {
        switch (*p) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*p < 0x20)
                    printf("\\u%04x", (unsigned) *p);
                else
                    putchar((int) *p);
                break;
        }
        p++;
    }
    putchar('"');
}

static int language_attributes(bool json) {
    size_t count = xr_public_attribute_count();
    if (json) {
        fputs("{\"schema_version\":1,\"count\":", stdout);
        printf("%zu,\"attributes\":[", count);
        for (size_t i = 0; i < count; i++) {
            const XrPublicAttributeInfo *info = xr_public_attribute_at(i);
            if (i)
                putchar(',');
            fputs("{\"spelling\":", stdout);
            print_json_string(info->spelling);
            fputs(",\"targets\":", stdout);
            print_json_string(info->targets);
            fputs(",\"arguments\":", stdout);
            print_json_string(info->arguments);
            fputs(",\"phase\":", stdout);
            print_json_string(info->phase);
            printf(",\"production\":%s,\"impact\":", info->production ? "true" : "false");
            print_json_string(info->impact);
            fputs(",\"stability\":", stdout);
            print_json_string(info->stability);
            putchar('}');
        }
        fputs("]}\n", stdout);
        return XR_CLI_EXIT_OK;
    }

    printf("Public attributes (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        const XrPublicAttributeInfo *info = xr_public_attribute_at(i);
        printf("  @%-12s targets=%-23s args=%s\n", info->spelling, info->targets, info->arguments);
        printf("                 phase=%s production=%s stability=%s\n", info->phase,
               info->production ? "yes" : "no", info->stability);
        printf("                 impact=%s\n", info->impact);
    }
    return XR_CLI_EXIT_OK;
}

XR_FUNC int cmd_language(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count != 1) {
        xr_cli_error("language", "expected subcommand 'attributes'");
        return XR_CLI_EXIT_USAGE;
    }
    if (strcmp(inv->positionals[0], "attributes") != 0) {
        xr_cli_error("language", "unknown subcommand '%s'", inv->positionals[0]);
        return XR_CLI_EXIT_USAGE;
    }
    return language_attributes(xr_cli_opt_bool(&inv->options, "json"));
}
