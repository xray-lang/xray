/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_link.c - AOT link manifest
 */

#include "xaot_link.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"

#include <stdio.h>
#include <string.h>

static void xaot_link_string_list_free(char **items, uint32_t count) {
    uint32_t i;

    if (!items)
        return;

    for (i = 0; i < count; i++)
        xr_free(items[i]);
    xr_free(items);
}

static bool xaot_link_select_list(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                  char ****out_items, uint32_t **out_count) {
    if (!manifest || !out_items || !out_count)
        return false;

    switch (kind) {
        case XAOT_LINK_GENERATED_C_FILE:
            *out_items = &manifest->generated_c_files;
            *out_count = &manifest->n_generated_c_files;
            return true;
        case XAOT_LINK_RUNTIME_CAP:
            *out_items = &manifest->runtime_caps;
            *out_count = &manifest->n_runtime_caps;
            return true;
        case XAOT_LINK_RUNTIME_OBJECT:
            *out_items = &manifest->runtime_objects;
            *out_count = &manifest->n_runtime_objects;
            return true;
        case XAOT_LINK_STDLIB_OBJECT:
            *out_items = &manifest->stdlib_objects;
            *out_count = &manifest->n_stdlib_objects;
            return true;
        case XAOT_LINK_STDLIB_SYMBOL:
            *out_items = &manifest->stdlib_symbols;
            *out_count = &manifest->n_stdlib_symbols;
            return true;
        case XAOT_LINK_SYSTEM_LIB:
            *out_items = &manifest->system_libs;
            *out_count = &manifest->n_system_libs;
            return true;
        case XAOT_LINK_DEFINE:
            *out_items = &manifest->defines;
            *out_count = &manifest->n_defines;
            return true;
        case XAOT_LINK_CC_FLAG:
            *out_items = &manifest->cc_flags;
            *out_count = &manifest->n_cc_flags;
            return true;
        case XAOT_LINK_LD_FLAG:
            *out_items = &manifest->ld_flags;
            *out_count = &manifest->n_ld_flags;
            return true;
        default:
            return false;
    }
}

static bool xaot_link_list_for_kind(const XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                    char ***out_items, uint32_t *out_count) {
    if (!manifest || !out_items || !out_count)
        return false;

    switch (kind) {
        case XAOT_LINK_GENERATED_C_FILE:
            *out_items = manifest->generated_c_files;
            *out_count = manifest->n_generated_c_files;
            return true;
        case XAOT_LINK_RUNTIME_CAP:
            *out_items = manifest->runtime_caps;
            *out_count = manifest->n_runtime_caps;
            return true;
        case XAOT_LINK_RUNTIME_OBJECT:
            *out_items = manifest->runtime_objects;
            *out_count = manifest->n_runtime_objects;
            return true;
        case XAOT_LINK_STDLIB_OBJECT:
            *out_items = manifest->stdlib_objects;
            *out_count = manifest->n_stdlib_objects;
            return true;
        case XAOT_LINK_STDLIB_SYMBOL:
            *out_items = manifest->stdlib_symbols;
            *out_count = manifest->n_stdlib_symbols;
            return true;
        case XAOT_LINK_SYSTEM_LIB:
            *out_items = manifest->system_libs;
            *out_count = manifest->n_system_libs;
            return true;
        case XAOT_LINK_DEFINE:
            *out_items = manifest->defines;
            *out_count = manifest->n_defines;
            return true;
        case XAOT_LINK_CC_FLAG:
            *out_items = manifest->cc_flags;
            *out_count = manifest->n_cc_flags;
            return true;
        case XAOT_LINK_LD_FLAG:
            *out_items = manifest->ld_flags;
            *out_count = manifest->n_ld_flags;
            return true;
        default:
            return false;
    }
}

static bool xaot_json_write_raw(FILE *out, const char *s) {
    return fputs(s, out) >= 0;
}

static bool xaot_json_write_string(FILE *out, const char *s) {
    const unsigned char *p;
    unsigned int ch;

    if (!s)
        return xaot_json_write_raw(out, "null");

    if (fputc('"', out) == EOF)
        return false;

    p = (const unsigned char *) s;
    while (*p) {
        ch = (unsigned int) *p;
        switch (ch) {
            case '"':
                if (!xaot_json_write_raw(out, "\\\""))
                    return false;
                break;
            case '\\':
                if (!xaot_json_write_raw(out, "\\\\"))
                    return false;
                break;
            case '\b':
                if (!xaot_json_write_raw(out, "\\b"))
                    return false;
                break;
            case '\f':
                if (!xaot_json_write_raw(out, "\\f"))
                    return false;
                break;
            case '\n':
                if (!xaot_json_write_raw(out, "\\n"))
                    return false;
                break;
            case '\r':
                if (!xaot_json_write_raw(out, "\\r"))
                    return false;
                break;
            case '\t':
                if (!xaot_json_write_raw(out, "\\t"))
                    return false;
                break;
            default:
                if (ch < 0x20) {
                    if (fprintf(out, "\\u%04x", ch) < 0)
                        return false;
                } else if (fputc((int) ch, out) == EOF) {
                    return false;
                }
                break;
        }
        p++;
    }

    return fputc('"', out) != EOF;
}

static bool xaot_json_write_string_array(FILE *out, const char *name, char **items, uint32_t count,
                                         bool trailing_comma) {
    uint32_t i;

    if (!items && count > 0)
        return false;

    if (fprintf(out, "  \"%s\": [", name) < 0)
        return false;

    for (i = 0; i < count; i++) {
        if (i > 0 && !xaot_json_write_raw(out, ", "))
            return false;
        if (!xaot_json_write_string(out, items[i]))
            return false;
    }

    if (!xaot_json_write_raw(out, trailing_comma ? "],\n" : "]\n"))
        return false;

    return true;
}

static bool xaot_json_write_target(FILE *out, const XaotTarget *target) {
    bool ok;

    if (!target)
        return false;

    ok = true;
    ok = ok && xaot_json_write_raw(out, "  \"target\": {\n");
    ok = ok && xaot_json_write_raw(out, "    \"name\": ");
    ok = ok && xaot_json_write_string(out, target->name);
    ok = ok && xaot_json_write_raw(out, ",\n");
    ok = ok && xaot_json_write_raw(out, "    \"arch\": ");
    ok = ok && xaot_json_write_string(out, target->arch);
    ok = ok && xaot_json_write_raw(out, ",\n");
    ok = ok && xaot_json_write_raw(out, "    \"os\": ");
    ok = ok && xaot_json_write_string(out, target->os);
    ok = ok && xaot_json_write_raw(out, ",\n");
    ok = ok && xaot_json_write_raw(out, "    \"abi\": ");
    ok = ok && xaot_json_write_string(out, target->abi);
    ok = ok && xaot_json_write_raw(out, ",\n");
    ok = ok && xaot_json_write_raw(out, "    \"object_format\": ");
    ok = ok && xaot_json_write_string(out, target->object_format);
    ok = ok && xaot_json_write_raw(out, ",\n");
    ok = ok && xaot_json_write_raw(out, "    \"triple\": ");
    ok = ok && xaot_json_write_string(out, target->triple);
    ok = ok && xaot_json_write_raw(out, "\n");
    ok = ok && xaot_json_write_raw(out, "  },\n");
    return ok;
}

static bool xaot_target_profile(const char *name, const char **arch, const char **os,
                                const char **abi, const char **object_format, const char **triple) {
    if (!name || strcmp(name, "native") == 0 || strcmp(name, "native-c90") == 0) {
        *arch = "native";
        *os = "native";
        *abi = "host";
        *object_format = "native";
        *triple = "native";
        return true;
    }
    if (strcmp(name, "x86_64-linux-musl") == 0) {
        *arch = "x86_64";
        *os = "linux";
        *abi = "musl";
        *object_format = "elf";
        *triple = "x86_64-linux-musl";
        return true;
    }
    if (strcmp(name, "aarch64-linux-musl") == 0) {
        *arch = "aarch64";
        *os = "linux";
        *abi = "musl";
        *object_format = "elf";
        *triple = "aarch64-linux-musl";
        return true;
    }
    if (strcmp(name, "x86_64-windows-gnu") == 0) {
        *arch = "x86_64";
        *os = "windows";
        *abi = "gnu";
        *object_format = "coff";
        *triple = "x86_64-windows-gnu";
        return true;
    }
    if (strcmp(name, "aarch64-windows-gnu") == 0) {
        *arch = "aarch64";
        *os = "windows";
        *abi = "gnu";
        *object_format = "coff";
        *triple = "aarch64-windows-gnu";
        return true;
    }
    return false;
}

XR_FUNC bool xaot_target_init(XaotTarget *target, const char *name) {
    const char *resolved_name;
    const char *arch;
    const char *os;
    const char *abi;
    const char *object_format;
    const char *triple;

    if (!target)
        return false;

    resolved_name = name ? name : "native-c90";
    if (!xaot_target_profile(resolved_name, &arch, &os, &abi, &object_format, &triple)) {
        arch = "unknown";
        os = "unknown";
        abi = "unknown";
        object_format = "unknown";
        triple = resolved_name;
    }
    return xaot_target_init_ex(target, resolved_name, arch, os, abi, object_format, triple);
}

XR_FUNC bool xaot_target_init_ex(XaotTarget *target, const char *name, const char *arch,
                                 const char *os, const char *abi, const char *object_format,
                                 const char *triple) {
    if (!target)
        return false;

    memset(target, 0, sizeof(*target));
    target->name = xr_strdup(name ? name : "native");
    target->arch = xr_strdup(arch ? arch : "unknown");
    target->os = xr_strdup(os ? os : "unknown");
    target->abi = xr_strdup(abi ? abi : "unknown");
    target->object_format = xr_strdup(object_format ? object_format : "unknown");
    target->triple = xr_strdup(triple ? triple : (name ? name : "native"));
    if (!target->name || !target->arch || !target->os || !target->abi || !target->object_format ||
        !target->triple) {
        xaot_target_free(target);
        return false;
    }
    return true;
}

XR_FUNC void xaot_target_free(XaotTarget *target) {
    if (!target)
        return;

    xr_free(target->name);
    xr_free(target->arch);
    xr_free(target->os);
    xr_free(target->abi);
    xr_free(target->object_format);
    xr_free(target->triple);
    memset(target, 0, sizeof(*target));
}

static bool xaot_target_copy_init(XaotTarget *out, const XaotTarget *target) {
    if (!target)
        return xaot_target_init(out, NULL);
    return xaot_target_init_ex(out, target->name, target->arch, target->os, target->abi,
                               target->object_format, target->triple);
}

XR_FUNC bool xaot_link_manifest_init(XaotLinkManifest *manifest, const XaotTarget *target) {
    if (!manifest)
        return false;

    memset(manifest, 0, sizeof(*manifest));
    if (!xaot_target_copy_init(&manifest->target, target)) {
        memset(manifest, 0, sizeof(*manifest));
        return false;
    }

    return true;
}

XR_FUNC void xaot_link_manifest_free(XaotLinkManifest *manifest) {
    if (!manifest)
        return;

    xaot_target_free(&manifest->target);
    xaot_link_string_list_free(manifest->generated_c_files, manifest->n_generated_c_files);
    xaot_link_string_list_free(manifest->runtime_caps, manifest->n_runtime_caps);
    xaot_link_string_list_free(manifest->runtime_objects, manifest->n_runtime_objects);
    xaot_link_string_list_free(manifest->stdlib_objects, manifest->n_stdlib_objects);
    xaot_link_string_list_free(manifest->stdlib_symbols, manifest->n_stdlib_symbols);
    xaot_link_string_list_free(manifest->system_libs, manifest->n_system_libs);
    xaot_link_string_list_free(manifest->defines, manifest->n_defines);
    xaot_link_string_list_free(manifest->cc_flags, manifest->n_cc_flags);
    xaot_link_string_list_free(manifest->ld_flags, manifest->n_ld_flags);
    memset(manifest, 0, sizeof(*manifest));
}

XR_FUNC bool xaot_link_manifest_set_target(XaotLinkManifest *manifest, const XaotTarget *target) {
    XaotTarget copy;

    if (!manifest || !target)
        return false;
    if (!xaot_target_copy_init(&copy, target))
        return false;
    xaot_target_free(&manifest->target);
    manifest->target = copy;
    return true;
}

XR_FUNC bool xaot_link_manifest_add(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                    const char *value) {
    char ***items;
    uint32_t *count;
    char **new_items;
    char *copy;
    size_t new_count;

    if (!value)
        return false;
    if (!xaot_link_select_list(manifest, kind, &items, &count))
        return false;
    if (*count == UINT32_MAX)
        return false;

    copy = xr_strdup(value);
    if (!copy)
        return false;

    new_count = (size_t) *count + 1;
    new_items = (char **) xr_realloc(*items, new_count * sizeof(char *));
    if (!new_items) {
        xr_free(copy);
        return false;
    }

    *items = new_items;
    (*items)[*count] = copy;
    *count = *count + 1;
    return true;
}

XR_FUNC bool xaot_link_manifest_add_unique(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                           const char *value) {
    if (!value)
        return false;
    if (xaot_link_manifest_contains(manifest, kind, value))
        return true;
    return xaot_link_manifest_add(manifest, kind, value);
}

XR_FUNC bool xaot_link_manifest_contains(const XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                         const char *value) {
    char **items;
    uint32_t count;
    uint32_t i;

    if (!value)
        return false;
    if (!xaot_link_list_for_kind(manifest, kind, &items, &count))
        return false;
    if (!items && count > 0)
        return false;

    for (i = 0; i < count; i++) {
        if (items[i] && strcmp(items[i], value) == 0)
            return true;
    }
    return false;
}

XR_FUNC bool xaot_link_manifest_needs_runtime(const XaotLinkManifest *manifest) {
    return manifest && manifest->n_runtime_objects > 0;
}

XR_FUNC char *xaot_link_manifest_dump_json(const XaotLinkManifest *manifest) {
    char *buf;
    size_t bufsz;
    FILE *out;
    bool ok;

    if (!manifest)
        return NULL;

    buf = NULL;
    bufsz = 0;
    out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;

    ok = true;
    ok = ok && xaot_json_write_raw(out, "{\n");
    ok = ok && xaot_json_write_target(out, &manifest->target);
    ok = ok && xaot_json_write_string_array(out, "generated_c_files", manifest->generated_c_files,
                                            manifest->n_generated_c_files, true);
    ok = ok && xaot_json_write_string_array(out, "runtime_caps", manifest->runtime_caps,
                                            manifest->n_runtime_caps, true);
    ok = ok && xaot_json_write_string_array(out, "runtime_objects", manifest->runtime_objects,
                                            manifest->n_runtime_objects, true);
    ok = ok && xaot_json_write_string_array(out, "stdlib_objects", manifest->stdlib_objects,
                                            manifest->n_stdlib_objects, true);
    ok = ok && xaot_json_write_string_array(out, "stdlib_symbols", manifest->stdlib_symbols,
                                            manifest->n_stdlib_symbols, true);
    ok = ok && xaot_json_write_string_array(out, "system_libs", manifest->system_libs,
                                            manifest->n_system_libs, true);
    ok = ok &&
         xaot_json_write_string_array(out, "defines", manifest->defines, manifest->n_defines, true);
    ok = ok && xaot_json_write_string_array(out, "cc_flags", manifest->cc_flags,
                                            manifest->n_cc_flags, true);
    ok = ok && xaot_json_write_string_array(out, "ld_flags", manifest->ld_flags,
                                            manifest->n_ld_flags, false);
    ok = ok && xaot_json_write_raw(out, "}\n");

    if (!ok || ferror(out)) {
        (void) xr_close_memstream(out, &buf, &bufsz);
        xr_free(buf);
        return NULL;
    }

    if (xr_close_memstream(out, &buf, &bufsz) != 0)
        return NULL;

    return buf;
}
