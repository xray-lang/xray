/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_builtin_dump.c - Dump analyzer builtin metadata as JSON
 *
 * KEY CONCEPT:
 *   Knowledge generation needs stdlib symbol signatures from the same
 *   metadata table the analyzer uses. This command exposes that table in a
 *   deterministic machine-readable form without initialising the runtime.
 */

#include "xcli_spec.h"
#include "../../frontend/analyzer/xanalyzer_builtins.h"
#include "../../base/xchecks.h"
#include <stdio.h>

static void json_string(FILE *out, const char *s) {
    if (!s) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        switch (*p) {
            case '"':
                fputs("\\\"", out);
                break;
            case '\\':
                fputs("\\\\", out);
                break;
            case '\b':
                fputs("\\b", out);
                break;
            case '\f':
                fputs("\\f", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (*p < 0x20) {
                    fprintf(out, "\\u%04x", *p);
                } else {
                    fputc((int) *p, out);
                }
                break;
        }
    }
    fputc('"', out);
}

static void dump_symbol(FILE *out, const char *name, const char *signature, const char *summary,
                        const char *kind, bool *first) {
    if (!name || name[0] == '\0')
        return;
    if (!*first)
        fputs(",\n", out);
    *first = false;
    fputs("        {\"name\":", out);
    json_string(out, name);
    fputs(",\"signature\":", out);
    json_string(out, signature ? signature : "");
    fputs(",\"summary\":", out);
    json_string(out, summary ? summary : "");
    fputs(",\"kind\":", out);
    json_string(out, kind ? kind : "function");
    fputs("}", out);
}

static void dump_module(FILE *out, const XaBuiltinModule *mod, bool first_module) {
    XR_DCHECK(out != NULL, "dump_module: NULL out");
    XR_DCHECK(mod != NULL, "dump_module: NULL mod");
    if (!first_module)
        fputs(",\n", out);
    fputs("    {\"name\":", out);
    json_string(out, mod->name);
    fputs(",\"symbols\":[\n", out);

    bool first_symbol = true;
    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *fn = &mod->functions[i];
        dump_symbol(out, fn->name, fn->signature, fn->doc, "function", &first_symbol);
    }
    for (int i = 0; i < mod->handle_count; i++) {
        const XaBuiltinHandle *handle = &mod->handles[i];
        dump_symbol(out, handle->name, handle->name, "Handle type", "handle", &first_symbol);
        for (int j = 0; j < handle->field_count; j++) {
            const XaBuiltinHandleField *field = &handle->fields[j];
            char name_buf[160];
            char sig_buf[160];
            snprintf(name_buf, sizeof(name_buf), "%s.%s", handle->name, field->name);
            snprintf(sig_buf, sizeof(sig_buf), "%s%s", field->is_const ? "const " : "",
                     field->type_str ? field->type_str : "unknown");
            dump_symbol(out, name_buf, sig_buf, "Handle field", "field", &first_symbol);
        }
        for (int j = 0; j < handle->method_count; j++) {
            const XaBuiltinMember *method = &handle->methods[j];
            char name_buf[160];
            snprintf(name_buf, sizeof(name_buf), "%s.%s", handle->name, method->name);
            dump_symbol(out, name_buf, method->signature, method->doc, "method", &first_symbol);
        }
    }

    fputs("\n    ]}", out);
}

XR_FUNC int cmd_builtin_dump(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "cmd_builtin_dump: NULL inv");
    (void) inv;
    int count = xa_builtin_get_module_count();
    fputs("{\n  \"modules\": [\n", stdout);
    for (int i = 0; i < count; i++) {
        const XaBuiltinModule *mod = xa_builtin_get_module_at(i);
        if (mod)
            dump_module(stdout, mod, i == 0);
    }
    fputs("\n  ]\n}\n", stdout);
    return XR_CLI_EXIT_OK;
}
