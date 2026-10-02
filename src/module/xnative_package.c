/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_package.c - Native package manifest parser and verifier
 */

#include "xnative_package.h"
#include "../base/xfileio.h"
#include "xmanifest_owner.inc.h"
#include "../base/xsha256.h"
#include "../os/os_fs.h"

#include "../runtime/value/xstruct_layout.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const XrFreestandingEntryPlan *xr_native_package_find_entry_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                            const char *xray_name);
static void xr_native_package_explain_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, FILE *out);
static bool xr_native_package_validate_symbol_arity_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, const char *xray_name,
                                             uint32_t arity, char *errbuf, size_t errbuf_len);
static const XrNativeSymbol *xr_native_package_find_symbol_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                    const char *xray_name);
static const XrNativeUnit *xr_native_package_find_unit_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, const char *name);
static void xr_native_package_note_layout_subject_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *xray_type);
static const XrCExportPlan *xr_native_package_find_export_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                   const char *xray_name);
static XrNativePackagePlan *xr_native_package_plan_parse_impl(ManifestContext *ctx, XrTomlValue *toml_root,
                                                  const char *project_root);
static bool xr_native_package_configure_c_exports_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *public_prefix,
                                           const char *exclude_csv, char *error,
                                           size_t error_size);
static bool xr_native_package_resolve_layout_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *xray_type,
                                      const XrAggregateLayout *layout);
static const XrLinkSymbolPlan *xr_native_package_find_link_symbol_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                           const char *xray_name);

#define XR_NATIVE_FNV_OFFSET UINT64_C(1469598103934665603)
#define XR_NATIVE_FNV_PRIME UINT64_C(1099511628211)

static bool native_valid_c_identifier(ManifestContext *ctx, const char *name);

static uint64_t native_hash_bytes(ManifestContext *ctx, uint64_t hash, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *) data;
    for (size_t i = 0; manifest_work(ctx, 1) && (i < len); i++) {
        hash ^= bytes[i];
        hash *= XR_NATIVE_FNV_PRIME;
    }
    return hash;
}

static uint64_t native_hash_text(ManifestContext *ctx, uint64_t hash, const char *text) {
    if (!text)
        return native_hash_bytes(ctx, hash, "\xff", 1);
    hash = native_hash_bytes(ctx, hash, text, manifest_length(ctx, text));
    return native_hash_bytes(ctx, hash, "\0", 1);
}

static bool native_fail(ManifestContext *ctx, XrNativePackagePlan *plan, const char *fmt, ...) {
    va_list args;va_start(args,fmt);manifest_verror(ctx,fmt,args);va_end(args);
    if (plan) plan->valid=false;
    return false;
}

static char *native_dup_string(ManifestContext *ctx, XrTomlValue *table, const char *key) {
    const char *value = manifest_string(ctx, table, key);
    return value ? manifest_duplicate(ctx, value) : NULL;
}

static bool native_key_is(ManifestContext *ctx, const char *key, const char *const *allowed, size_t count) {
    if (!key)
        return false;
    for (size_t i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        if (manifest_compare(ctx, key, allowed[i]) == 0)
            return true;
    }
    return false;
}

static bool native_text_is(ManifestContext *ctx, const char *text, const char *const *allowed, size_t count) {
    return native_key_is(ctx, text, allowed, count);
}

static bool native_validate_keys(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table, const char *where,
                                 const char *const *allowed, size_t allowed_count) {
    if (!table || table->type != XR_TOML_TABLE)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s must be a table", where);
    for (int i = 0; manifest_work(ctx, 1) && (i < table->as.table.count); i++) {
        const char *key = table->as.table.members[i].key;
        if (!native_key_is(ctx, key, allowed, allowed_count))
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: unsupported field %s.%s", where,
                               key ? key : "?");
    }
    return true;
}

static void native_free_string_array(char **items, uint32_t count) {
    if (!items)
        return;
    for (uint32_t i = 0; i < count; i++)
        manifest_free(items[i]);
    manifest_free(items);
}

static bool native_parse_string_array(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table,
                                      const char *key, bool required, char ***out_items,
                                      uint32_t *out_count, const char *where) {
    XrTomlValue *array = manifest_get_type(ctx, table, key, XR_TOML_ARRAY);
    *out_items = NULL;
    *out_count = 0;
    if (!array) {
        if (required)
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s.%s is required", where, key);
        return true;
    }
    int count = xtoml_array_len(array);
    if (required && count == 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s.%s cannot be empty", where, key);
    if (count <= 0)
        return true;
    char **items = (char **) manifest_calloc(ctx, (size_t) count, sizeof(char *));
    if (!items)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate %s.%s", where, key);
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        XrTomlValue *item = xtoml_array_get(array, i);
        if (!item || item->type != XR_TOML_STRING || !item->as.string || !item->as.string[0]) {
            native_free_string_array(items, (uint32_t) count);
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s.%s[%d] must be a non-empty string", where,
                               key, i);
        }
        items[i] = manifest_duplicate(ctx, item->as.string);
        if (!items[i]) {
            native_free_string_array(items, (uint32_t) count);
            return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot copy %s.%s[%d]", where, key, i);
        }
    }
    *out_items = items;
    *out_count = (uint32_t) count;
    return true;
}

static bool native_safe_relative_path(ManifestContext *ctx, const char *path) {
    if (!path) return false;
    size_t segment_length=0;bool dots=true;unsigned char first=0;
    for (size_t i=0;;++i) {
        if (!manifest_work(ctx,1))return false;
        unsigned char byte=(unsigned char)path[i];
        if (!i)first=byte;
        if (i==1&&isalpha(first)&&byte==':')return false;
        if (!byte||byte=='/'||byte=='\\') {
            if (!segment_length||(dots&&segment_length<=2))return false;
            if (!byte)return true;
            segment_length=0;dots=true;
        } else { ++segment_length;if(byte!='.')dots=false; }
    }
}

static bool native_path_belongs_to_root(ManifestContext *ctx, const char *root, const char *path) {
    size_t root_len;
    if (!root || !path)
        return false;
    root_len = manifest_length(ctx, root);
    if (manifest_compare_n(ctx,root,path,root_len)!=0||!manifest_work(ctx,2))return false;
    char last=root_len?root[root_len-1]:0,next=path[root_len];
    return last=='/'||last=='\\'||!next||next=='/'||next=='\\';
}

static char *native_resolve_existing_path(ManifestContext *ctx, XrNativePackagePlan *plan, const char *relative,
                                          const char *where) {
    if (!native_safe_relative_path(ctx, relative)) {
        native_fail(ctx, plan, "E-NATIVE-PATH: %s must be a normalized package-relative path: %s", where,
                    relative ? relative : "");
        return NULL;
    }
    char *joined = manifest_join(ctx, plan->root, relative);
    char *resolved = joined ? manifest_realpath(ctx, joined) : NULL;
    manifest_free(joined);
    if (!resolved) {
        native_fail(ctx, plan, "E-NATIVE-PATH: %s does not exist: %s", where, relative);
        return NULL;
    }
    XrFsStat stat={0};
    if (!manifest_io(ctx,xr_os_io_stat(&ctx->policy,resolved,&stat))) {
        manifest_free(resolved);return NULL;
    }
    if (stat.kind!=XR_FS_FILE && stat.kind!=XR_FS_DIR) {
        native_fail(ctx,plan,"E-NATIVE-PATH: %s is not a file or directory",where);
        manifest_free(resolved);return NULL;
    }
    if (!native_path_belongs_to_root(ctx, plan->root, resolved)) {
        native_fail(ctx, plan, "E-NATIVE-PATH: %s escapes the package root: %s", where, relative);
        manifest_free(resolved);
        return NULL;
    }
    return resolved;
}

static char *native_resolve_output_path(ManifestContext *ctx, XrNativePackagePlan *plan, const char *relative,
                                        const char *where) {
    if (!relative)
        return NULL;
    if (!native_safe_relative_path(ctx, relative)) {
        native_fail(ctx, plan, "E-NATIVE-PATH: %s must be a normalized package-relative path: %s", where,
                    relative);
        return NULL;
    }
    return manifest_join(ctx, plan->root, relative);
}

static bool native_sha256_file(ManifestContext *ctx, const char *root, const char *logical, char output[65]) {
    XrFileBytes bytes={0};uint8_t digest[32];static const char hex[]="0123456789abcdef";
    if (!manifest_read_under_root(ctx,root,logical,SIZE_MAX-1,&bytes)) return false;
    if (bytes.size>UINT64_MAX-sizeof(digest))manifest_status(ctx,XR_MANIFEST_BUDGET);
    if (manifest_work(ctx,bytes.size+sizeof(digest)))xr_sha256((const uint8_t *)bytes.data,bytes.size,digest);
    manifest_free(bytes.data);
    for(size_t i=0;i<sizeof(digest);++i) {
        if(!manifest_work(ctx,3))return false;
        uint8_t byte=digest[i];output[2*i]=hex[byte>>4];output[2*i+1]=hex[byte&15];
    }
    if(!manifest_work(ctx,1))return false;output[64]=0;return true;
}

static bool native_valid_sha256(ManifestContext *ctx, const char *text) {
    if (!text || manifest_length(ctx, text) != 64)
        return false;
    for (size_t i = 0; manifest_work(ctx, 1) && (i < 64); i++) {
        unsigned char byte=(unsigned char)text[i];
        if (!isdigit(byte) && !(byte >= 'a' && byte <= 'f'))
            return false;
    }
    return true;
}

static bool native_valid_define(ManifestContext *ctx, const char *text) {
    if (!text||!manifest_work(ctx,1))return false;
    unsigned char byte=(unsigned char)*text++;
    if (!isalpha(byte)&&byte!='_')return false;
    bool value=false;
    for (;;) {
        if (!manifest_work(ctx,1))return false;
        byte=(unsigned char)*text++;
        if (!byte)return true;
        if (value) { if(byte<0x20)return false; }
        else if(byte=='=')value=true;
        else if(!isalnum(byte)&&byte!='_')return false;
    }
}

static bool native_parse_audit(ManifestContext *ctx, XrNativePackagePlan *plan, const char *value) {
    if (value && manifest_compare(ctx, value, "shipping") == 0)
        plan->audit_mode = XR_NATIVE_AUDIT_SHIPPING;
    else if (value && manifest_compare(ctx, value, "exploratory") == 0)
        plan->audit_mode = XR_NATIVE_AUDIT_EXPLORATORY;
    else
        return native_fail(ctx, plan,
                           "E-NATIVE-SCHEMA: native.audit_mode must be shipping or exploratory");
    return true;
}

static bool native_parse_vm_policy(ManifestContext *ctx, XrNativePackagePlan *plan, const char *value) {
    if (value && manifest_compare(ctx, value, "verified-dynamic") == 0)
        plan->vm_policy = XR_NATIVE_VM_VERIFIED_DYNAMIC;
    else if (value && manifest_compare(ctx, value, "unsupported") == 0)
        plan->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
    else
        return native_fail(ctx, plan,
                           "E-NATIVE-SCHEMA: native.vm must be verified-dynamic or unsupported");
    return true;
}

static XrNativeUnitKind native_unit_kind(ManifestContext *ctx, const char *value) {
    if (!value)
        return 0;
    if (manifest_compare(ctx, value, "c") == 0)
        return XR_NATIVE_UNIT_C;
    if (manifest_compare(ctx, value, "asm") == 0)
        return XR_NATIVE_UNIT_ASM;
    if (manifest_compare(ctx, value, "object") == 0)
        return XR_NATIVE_UNIT_OBJECT;
    if (manifest_compare(ctx, value, "static-library") == 0)
        return XR_NATIVE_UNIT_STATIC_LIBRARY;
    if (manifest_compare(ctx, value, "dynamic-library") == 0)
        return XR_NATIVE_UNIT_DYNAMIC_LIBRARY;
    if (manifest_compare(ctx, value, "platform") == 0)
        return XR_NATIVE_UNIT_PLATFORM;
    return 0;
}

static XrNativeSymbolKind native_symbol_kind(ManifestContext *ctx, const char *value) {
    if (value && manifest_compare(ctx, value, "function") == 0)
        return XR_NATIVE_SYMBOL_FUNCTION;
    if (value && manifest_compare(ctx, value, "address") == 0)
        return XR_NATIVE_SYMBOL_ADDRESS;
    return 0;
}

static XrNativeParamAccess native_access(ManifestContext *ctx, const char *value) {
    if (value && manifest_compare(ctx, value, "none") == 0)
        return XR_NATIVE_ACCESS_NONE;
    if (value && manifest_compare(ctx, value, "read") == 0)
        return XR_NATIVE_ACCESS_READ;
    if (value && manifest_compare(ctx, value, "write") == 0)
        return XR_NATIVE_ACCESS_WRITE;
    if (value && manifest_compare(ctx, value, "readwrite") == 0)
        return XR_NATIVE_ACCESS_READWRITE;
    return (XrNativeParamAccess) -1;
}

static XrNativeEscape native_escape(ManifestContext *ctx, const char *value) {
    if (value && manifest_compare(ctx, value, "noescape") == 0)
        return XR_NATIVE_ESCAPE_NOESCAPE;
    if (value && manifest_compare(ctx, value, "borrow") == 0)
        return XR_NATIVE_ESCAPE_BORROW;
    if (value && manifest_compare(ctx, value, "retain") == 0)
        return XR_NATIVE_ESCAPE_RETAIN;
    if (value && manifest_compare(ctx, value, "consume") == 0)
        return XR_NATIVE_ESCAPE_CONSUME;
    return XR_NATIVE_ESCAPE_UNSPECIFIED;
}

static XrNativeOwnership native_ownership(ManifestContext *ctx, const char *value) {
    if (value && manifest_compare(ctx, value, "value") == 0)
        return XR_NATIVE_OWNERSHIP_VALUE;
    if (value && manifest_compare(ctx, value, "borrowed") == 0)
        return XR_NATIVE_OWNERSHIP_BORROWED;
    if (value && manifest_compare(ctx, value, "owned") == 0)
        return XR_NATIVE_OWNERSHIP_OWNED;
    if (value && manifest_compare(ctx, value, "none") == 0)
        return XR_NATIVE_OWNERSHIP_NONE;
    return XR_NATIVE_OWNERSHIP_UNSPECIFIED;
}

static XrNativeOutputState native_output(ManifestContext *ctx, const char *value) {
    if (!value || manifest_compare(ctx, value, "none") == 0)
        return XR_NATIVE_OUTPUT_NONE;
    if (manifest_compare(ctx, value, "complete") == 0)
        return XR_NATIVE_OUTPUT_COMPLETE;
    if (manifest_compare(ctx, value, "partial") == 0)
        return XR_NATIVE_OUTPUT_PARTIAL;
    return (XrNativeOutputState) -1;
}

static bool native_parse_units(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const unit_keys[] = {
        "name",     "kind",         "sources",    "source_hashes", "include_dirs",
        "defines",  "system_links", "c_standard", "optimization",  "visibility",
        "warnings", "cpu_feature",  "output",     "purpose",
    };
    XrTomlValue *array = manifest_get_type(ctx, native, "unit", XR_TOML_ARRAY);
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: native.unit cannot be empty");
    plan->units = (XrNativeUnit *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeUnit));
    if (!plan->units)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate native units");
    plan->unit_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[64]={0};
        manifest_format(ctx, where, sizeof(where), "native.unit[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeUnit *unit = &plan->units[i];
        if (!native_validate_keys(ctx, plan, table, where, unit_keys,
                                  sizeof(unit_keys) / sizeof(unit_keys[0])))
            return false;
        unit->name = native_dup_string(ctx, table, "name");
        unit->kind = native_unit_kind(ctx, manifest_string(ctx, table, "kind"));
        unit->language_standard = native_dup_string(ctx, table, "c_standard");
        unit->optimization = native_dup_string(ctx, table, "optimization");
        unit->visibility = native_dup_string(ctx, table, "visibility");
        unit->warning_policy = native_dup_string(ctx, table, "warnings");
        unit->cpu_feature = native_dup_string(ctx, table, "cpu_feature");
        unit->purpose = native_dup_string(ctx, table, "purpose");
        if (!unit->name || !unit->name[0] || !unit->kind || !unit->purpose || !unit->purpose[0])
            return native_fail(ctx,
                plan, "E-NATIVE-SCHEMA: %s requires name, supported kind, and purpose", where);
        for (int j = 0; manifest_work(ctx, 1) && (j < i); j++) {
            if (manifest_compare(ctx, plan->units[j].name, unit->name) == 0)
                return native_fail(ctx, plan, "E-NATIVE-SCHEMA: duplicate native unit '%s'", unit->name);
        }
        if (unit->kind != XR_NATIVE_UNIT_PLATFORM) {
            if (!native_parse_string_array(ctx, plan, table, "sources", true, &unit->source_relpaths,
                                           &unit->source_count, where))
                return false;
            uint32_t hash_count = 0;
            if (!native_parse_string_array(ctx, plan, table, "source_hashes", true, &unit->source_hashes,
                                           &hash_count, where))
                return false;
            if (hash_count != unit->source_count) {
                native_free_string_array(unit->source_hashes,hash_count);unit->source_hashes=NULL;
                return native_fail(ctx,plan,"E-NATIVE-SCHEMA: %s.source_hashes must match sources",where);
            }
            unit->sources = (char **) manifest_calloc(ctx, (size_t) unit->source_count, sizeof(char *));
            if (!unit->sources)
                return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate %s.sources", where);
            uint64_t fingerprint = XR_NATIVE_FNV_OFFSET;
            for (uint32_t si = 0; manifest_work(ctx, 1) && (si < unit->source_count); si++) {
                char source_where[96]={0};
                char actual[65];
                manifest_format(ctx, source_where, sizeof(source_where), "%s.sources[%u]", where, si);
                unit->sources[si] =
                    native_resolve_existing_path(ctx, plan, unit->source_relpaths[si], source_where);
                if (!unit->sources[si])
                    return false;
                if (!native_valid_sha256(ctx, unit->source_hashes[si]))
                    return native_fail(ctx, plan,
                                       "E-NATIVE-SCHEMA: %s.source_hashes[%u] is not lower-case "
                                       "SHA-256",
                                       where, si);
                if (!native_sha256_file(ctx, plan->root, unit->source_relpaths[si], actual))
                    return native_fail(ctx, plan, "E-NATIVE-HASH: cannot hash %s",
                                       unit->source_relpaths[si]);
                if (manifest_compare(ctx, actual, unit->source_hashes[si]) != 0)
                    return native_fail(ctx, plan,
                                       "E-NATIVE-HASH-MISMATCH: %s differs from audited hash "
                                       "(expected %s, actual %s)",
                                       unit->source_relpaths[si], unit->source_hashes[si], actual);
                fingerprint = native_hash_text(ctx, fingerprint, unit->source_relpaths[si]);
                fingerprint = native_hash_text(ctx, fingerprint, actual);
            }
            unit->fingerprint = fingerprint ? fingerprint : 1;
        }
        uint32_t rel_include_count = 0;
        char **rel_includes = NULL;
        if (!native_parse_string_array(ctx, plan, table, "include_dirs", false, &rel_includes,
                                       &rel_include_count, where))
            return false;
        if (rel_include_count > 0) {
            unit->include_dirs = (char **) manifest_calloc(ctx, (size_t) rel_include_count, sizeof(char *));
            if (!unit->include_dirs) {
                native_free_string_array(rel_includes, rel_include_count);
                return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate include dirs");
            }
            unit->include_dir_count = rel_include_count;
            for (uint32_t di = 0; manifest_work(ctx, 1) && (di < rel_include_count); di++) {
                char include_where[96]={0};
                manifest_format(ctx, include_where, sizeof(include_where), "%s.include_dirs[%u]", where, di);
                unit->include_dirs[di] =
                    native_resolve_existing_path(ctx, plan, rel_includes[di], include_where);
                if (!unit->include_dirs[di]) {
                    native_free_string_array(rel_includes, rel_include_count);
                    return false;
                }
            }
        }
        native_free_string_array(rel_includes, rel_include_count);
        if (!native_parse_string_array(ctx, plan, table, "defines", false, &unit->defines,
                                       &unit->define_count, where) ||
            !native_parse_string_array(ctx, plan, table, "system_links", false, &unit->system_links,
                                       &unit->system_link_count, where))
            return false;
        for (uint32_t di = 0; manifest_work(ctx, 1) && (di < unit->define_count); di++) {
            if (!native_valid_define(ctx, unit->defines[di]))
                return native_fail(ctx, plan, "E-NATIVE-FLAG: unsafe define in %s: %s", where,
                                   unit->defines[di]);
        }
        const char *output = manifest_string(ctx, table, "output");
        if (output) {
            char output_where[80]={0};
            manifest_format(ctx, output_where, sizeof(output_where), "%s.output", where);
            unit->output = native_resolve_output_path(ctx, plan, output, output_where);
            if (!unit->output)
                return false;
        }
        if ((unit->kind == XR_NATIVE_UNIT_C || unit->kind == XR_NATIVE_UNIT_ASM) &&
            (!unit->output || !unit->output[0]))
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s requires an explicit derived output path",
                               where);
        if ((unit->kind == XR_NATIVE_UNIT_OBJECT || unit->kind == XR_NATIVE_UNIT_STATIC_LIBRARY ||
             unit->kind == XR_NATIVE_UNIT_DYNAMIC_LIBRARY) &&
            unit->source_count != 1)
            return native_fail(ctx, plan,
                               "E-NATIVE-SCHEMA: %s prebuilt unit requires exactly one audited "
                               "source artifact",
                               where);
        if (unit->kind == XR_NATIVE_UNIT_C &&
            (!unit->language_standard || (manifest_compare(ctx, unit->language_standard, "c11") != 0 &&
                                          manifest_compare(ctx, unit->language_standard, "c17") != 0 &&
                                          manifest_compare(ctx, unit->language_standard, "c23") != 0)))
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s.c_standard must be c11, c17, or c23",
                               where);
        if (!unit->optimization ||
            (manifest_compare(ctx, unit->optimization, "none") != 0 && manifest_compare(ctx, unit->optimization, "size") != 0 &&
             manifest_compare(ctx, unit->optimization, "release") != 0))
            return native_fail(ctx,
                plan, "E-NATIVE-FLAG: %s.optimization must be none, size, or release", where);
        if (!unit->visibility ||
            (manifest_compare(ctx, unit->visibility, "hidden") != 0 && manifest_compare(ctx, unit->visibility, "default") != 0))
            return native_fail(ctx, plan, "E-NATIVE-FLAG: %s.visibility must be hidden or default",
                               where);
        if (!unit->warning_policy || (manifest_compare(ctx, unit->warning_policy, "strict") != 0 &&
                                      manifest_compare(ctx, unit->warning_policy, "system") != 0))
            return native_fail(ctx, plan, "E-NATIVE-FLAG: %s.warnings must be strict or system", where);
        if (unit->cpu_feature && manifest_compare(ctx, unit->cpu_feature, "baseline") != 0)
            return native_fail(ctx,
                plan, "E-NATIVE-FLAG: %s.cpu_feature must be the sealed value baseline", where);
        {
            uint64_t fingerprint = unit->fingerprint ? unit->fingerprint : XR_NATIVE_FNV_OFFSET;
            fingerprint = native_hash_bytes(ctx, fingerprint, &unit->kind, sizeof(unit->kind));
            for (uint32_t di = 0; manifest_work(ctx, 1) && (di < unit->include_dir_count); di++)
                fingerprint = native_hash_text(ctx, fingerprint, unit->include_dirs[di]);
            for (uint32_t di = 0; manifest_work(ctx, 1) && (di < unit->define_count); di++)
                fingerprint = native_hash_text(ctx, fingerprint, unit->defines[di]);
            for (uint32_t li = 0; manifest_work(ctx, 1) && (li < unit->system_link_count); li++)
                fingerprint = native_hash_text(ctx, fingerprint, unit->system_links[li]);
            fingerprint = native_hash_text(ctx, fingerprint, unit->language_standard);
            fingerprint = native_hash_text(ctx, fingerprint, unit->optimization);
            fingerprint = native_hash_text(ctx, fingerprint, unit->visibility);
            fingerprint = native_hash_text(ctx, fingerprint, unit->warning_policy);
            fingerprint = native_hash_text(ctx, fingerprint, unit->cpu_feature);
            unit->fingerprint = fingerprint ? fingerprint : 1;
        }
    }
    return true;
}

static bool native_parse_param_contract(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table,
                                        XrNativeParamContract *param, uint32_t expected_index,
                                        const char *where) {
    static const char *const keys[] = {
        "index",
        "access",
        "nullable",
        "length_from",
        "escape",
        "ownership",
        "output",
        "descriptor_rebind",
        "may_relocate",
        "may_shorten",
        "invalidates_views",
    };
    if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    int64_t index = manifest_integer(ctx, table, "index", -1);
    if (index < 0 || (uint64_t) index != expected_index)
        return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s.index must be %u", where, expected_index);
    param->index = expected_index;
    param->access = native_access(ctx, manifest_string(ctx, table, "access"));
    param->escape = native_escape(ctx, manifest_string(ctx, table, "escape"));
    param->ownership = native_ownership(ctx, manifest_string(ctx, table, "ownership"));
    param->output = native_output(ctx, manifest_string(ctx, table, "output"));
    XrTomlValue *nullable = manifest_get(ctx, table, "nullable");
    if ((int) param->access < 0 || !param->escape || !param->ownership || (int) param->output < 0 ||
        !nullable || nullable->type != XR_TOML_BOOL)
        return native_fail(ctx, plan,
                           "E-NATIVE-CONTRACT: %s requires typed access, nullable, escape, "
                           "ownership, and output",
                           where);
    param->nullable = nullable->as.boolean;
    int64_t length_from=manifest_integer(ctx,table,"length_from",-1);
    if (length_from < -1 || length_from > INT32_MAX)
        return native_fail(ctx,plan,"E-NATIVE-CONTRACT: %s.length_from is out of range",where);
    param->length_from=(int32_t)length_from;
    param->descriptor_rebind = manifest_boolean(ctx, table, "descriptor_rebind", false);
    param->may_relocate = manifest_boolean(ctx, table, "may_relocate", false);
    param->may_shorten = manifest_boolean(ctx, table, "may_shorten", false);
    param->invalidates_views = manifest_boolean(ctx, table, "invalidates_views", false);
    return true;
}

static bool native_parse_return_contract(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table,
                                         XrNativeReturnContract *result, const char *where) {
    static const char *const keys[] = {"ownership", "nullable", "validity", "drop_function"};
    if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    result->ownership = native_ownership(ctx, manifest_string(ctx, table, "ownership"));
    XrTomlValue *nullable = manifest_get(ctx, table, "nullable");
    result->validity = native_dup_string(ctx, table, "validity");
    result->drop_function = native_dup_string(ctx, table, "drop_function");
    if (!result->ownership || !nullable || nullable->type != XR_TOML_BOOL || !result->validity ||
        !result->validity[0])
        return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s requires ownership, nullable, and validity",
                           where);
    result->nullable = nullable->as.boolean;
    if (result->ownership == XR_NATIVE_OWNERSHIP_OWNED && !result->drop_function)
        return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s owned return requires drop_function",
                           where);
    return true;
}

static bool native_parse_callback_contracts(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table,
                                            XrNativeSymbolContract *contract, uint32_t param_count,
                                            const char *where) {
    static const char *const keys[] = {
        "index", "context_index", "escape", "thread", "lifetime", "runtime_attach", "reentrant",
    };
    XrTomlValue *array = manifest_get_type(ctx, table, "callbacks", XR_TOML_ARRAY);
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return true;
    contract->callbacks =
        (XrNativeCallbackContract *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeCallbackContract));
    if (!contract->callbacks)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate %s.callbacks", where);
    contract->callback_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        XrTomlValue *entry = xtoml_array_get(array, i);
        XrNativeCallbackContract *callback = &contract->callbacks[i];
        char callback_where[128]={0};
        manifest_format(ctx, callback_where, sizeof(callback_where), "%s.callbacks[%d]", where, i);
        if (!native_validate_keys(ctx, plan, entry, callback_where, keys,
                                  sizeof(keys) / sizeof(keys[0])))
            return false;
        int64_t index = manifest_integer(ctx, entry, "index", -1);
        int64_t context_index = manifest_integer(ctx, entry, "context_index", -1);
        XrTomlValue *reentrant = manifest_get(ctx, entry, "reentrant");
        const char *thread = manifest_string(ctx, entry, "thread");
        const char *lifetime = manifest_string(ctx, entry, "lifetime");
        const char *attach = manifest_string(ctx, entry, "runtime_attach");
        callback->escape = native_escape(ctx, manifest_string(ctx, entry, "escape"));
        if (index < 0 || (uint64_t) index >= param_count || context_index < -1 ||
            (context_index >= 0 && (uint64_t) context_index >= param_count) || !callback->escape ||
            !reentrant || reentrant->type != XR_TOML_BOOL)
            return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s is incomplete", callback_where);
        callback->index = (uint32_t) index;
        callback->context_index = (int32_t) context_index;
        callback->reentrant = reentrant->as.boolean;
        if (thread && manifest_compare(ctx, thread, "caller") == 0)
            callback->thread = XR_NATIVE_CALLBACK_CALLER_THREAD;
        else if (thread && manifest_compare(ctx, thread, "foreign") == 0)
            callback->thread = XR_NATIVE_CALLBACK_FOREIGN_THREAD;
        else
            return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s.thread must be caller or foreign",
                               callback_where);
        if (lifetime && manifest_compare(ctx, lifetime, "call") == 0)
            callback->lifetime = XR_NATIVE_CALLBACK_CALL_ONLY;
        else if (lifetime && manifest_compare(ctx, lifetime, "retained") == 0)
            callback->lifetime = XR_NATIVE_CALLBACK_RETAINED;
        else
            return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s.lifetime must be call or retained",
                               callback_where);
        if (attach && manifest_compare(ctx, attach, "not-required") == 0)
            callback->runtime_attach = XR_NATIVE_RUNTIME_ATTACH_NOT_REQUIRED;
        else if (attach && manifest_compare(ctx, attach, "attach-detach") == 0)
            callback->runtime_attach = XR_NATIVE_RUNTIME_ATTACH_DETACH;
        else
            return native_fail(ctx,
                plan, "E-NATIVE-CONTRACT: %s.runtime_attach must be not-required or attach-detach",
                callback_where);
        if (callback->thread == XR_NATIVE_CALLBACK_FOREIGN_THREAD &&
            callback->runtime_attach != XR_NATIVE_RUNTIME_ATTACH_DETACH)
            return native_fail(ctx, plan,
                               "E-NATIVE-CONTRACT: foreign-thread callback %u requires an "
                               "attach-detach runtime plan",
                               callback->index);
        if (callback->lifetime == XR_NATIVE_CALLBACK_RETAINED &&
            callback->escape != XR_NATIVE_ESCAPE_RETAIN)
            return native_fail(ctx, plan,
                               "E-NATIVE-CONTRACT: retained callback %u requires escape=retain",
                               callback->index);
    }
    return true;
}

static bool native_parse_symbol_contract(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *table,
                                         XrNativeSymbolContract *contract, const char *where) {
    static const char *const keys[] = {
        "params",   "return",  "effects", "callbacks", "failure", "allocation",
        "blocking", "suspend", "io",      "sync",      "panic",   "error",
    };
    if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    XrTomlValue *params = manifest_get_type(ctx, table, "params", XR_TOML_ARRAY);
    XrTomlValue *result = manifest_get_type(ctx, table, "return", XR_TOML_TABLE);
    if (!params || !result)
        return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s requires params and return", where);
    int count = xtoml_array_len(params);
    if (count > 0) {
        contract->params =
            (XrNativeParamContract *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeParamContract));
        if (!contract->params)
            return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate %s.params", where);
        contract->param_count = (uint32_t) count;
    }
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char param_where[112]={0};
        manifest_format(ctx, param_where, sizeof(param_where), "%s.params[%d]", where, i);
        if (!native_parse_param_contract(ctx, plan, xtoml_array_get(params, i), &contract->params[i],
                                         (uint32_t) i, param_where))
            return false;
    }
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        XrNativeParamContract *param = &contract->params[i];
        if (param->length_from >= count || param->length_from == i)
            return native_fail(ctx, plan,
                               "E-NATIVE-CONTRACT: %s.params[%d].length_from must name another "
                               "parameter or be omitted",
                               where, i);
        if (param->output != XR_NATIVE_OUTPUT_NONE && param->access != XR_NATIVE_ACCESS_WRITE &&
            param->access != XR_NATIVE_ACCESS_READWRITE)
            return native_fail(ctx, plan,
                               "E-NATIVE-CONTRACT: %s.params[%d] output requires write or "
                               "readwrite access",
                               where, i);
        if (param->output == XR_NATIVE_OUTPUT_PARTIAL &&
            plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
            return native_fail(ctx, plan,
                               "E-NATIVE-CONTRACT: shipping output parameter %d is partial; "
                               "typed materialization requires complete validity evidence",
                               i);
    }
    char return_where[112]={0};
    manifest_format(ctx, return_where, sizeof(return_where), "%s.return", where);
    if (!native_parse_return_contract(ctx, plan, result, &contract->result, return_where))
        return false;
    if (!native_parse_string_array(ctx, plan, table, "effects", true, &contract->effects,
                                   &contract->effect_count, where) ||
        !native_parse_callback_contracts(ctx, plan, table, contract, (uint32_t) count, where))
        return false;
    static const char *const allowed_effects[] = {
        "foreign", "alloc", "may_block", "suspend", "io", "sync", "panic", "abort",
    };
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < contract->effect_count); i++) {
        if (!native_text_is(ctx, contract->effects[i], allowed_effects,
                            sizeof(allowed_effects) / sizeof(allowed_effects[0])))
            return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s.effects[%u] is not a sealed effect",
                               where, i);
    }
    contract->failure = native_dup_string(ctx, table, "failure");
    contract->allocation = native_dup_string(ctx, table, "allocation");
    contract->blocking = native_dup_string(ctx, table, "blocking");
    contract->suspend = native_dup_string(ctx, table, "suspend");
    contract->io = native_dup_string(ctx, table, "io");
    contract->sync = native_dup_string(ctx, table, "sync");
    contract->panic = native_dup_string(ctx, table, "panic");
    contract->error = native_dup_string(ctx, table, "error");
    if (!contract->failure || !contract->allocation || !contract->blocking || !contract->suspend ||
        !contract->io || !contract->sync || !contract->panic || !contract->error)
        return native_fail(ctx, plan,
                           "E-NATIVE-CONTRACT: %s requires failure/allocation/blocking/suspend/"
                           "io/sync/panic/error",
                           where);
    static const char *const failure_values[] = {"none", "status_nonzero", "null", "errno"};
    static const char *const allocation_values[] = {"none", "may"};
    static const char *const binary_values[] = {"never", "may"};
    static const char *const io_values[] = {"none", "read", "write", "readwrite"};
    static const char *const sync_values[] = {"none", "internal", "external"};
    static const char *const panic_values[] = {"never", "abort"};
    static const char *const error_values[] = {"none", "status", "errno", "result"};
    if (!native_text_is(ctx, contract->failure, failure_values,
                        sizeof(failure_values) / sizeof(failure_values[0])) ||
        !native_text_is(ctx, contract->allocation, allocation_values,
                        sizeof(allocation_values) / sizeof(allocation_values[0])) ||
        !native_text_is(ctx, contract->blocking, binary_values,
                        sizeof(binary_values) / sizeof(binary_values[0])) ||
        !native_text_is(ctx, contract->suspend, binary_values,
                        sizeof(binary_values) / sizeof(binary_values[0])) ||
        !native_text_is(ctx, contract->io, io_values, sizeof(io_values) / sizeof(io_values[0])) ||
        !native_text_is(ctx, contract->sync, sync_values,
                        sizeof(sync_values) / sizeof(sync_values[0])) ||
        !native_text_is(ctx, contract->panic, panic_values,
                        sizeof(panic_values) / sizeof(panic_values[0])) ||
        !native_text_is(ctx, contract->error, error_values,
                        sizeof(error_values) / sizeof(error_values[0])))
        return native_fail(ctx, plan, "E-NATIVE-CONTRACT: %s contains an unknown typed contract value",
                           where);
    contract->complete = true;
    return true;
}

static bool native_parse_symbols(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const symbol_keys[] = {"xray", "native",  "kind", "calling_convention",
                                              "unit", "contract"};
    XrTomlValue *array = manifest_get_type(ctx, native, "symbol", XR_TOML_ARRAY);
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: native.symbol cannot be empty");
    plan->symbols = (XrNativeSymbol *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeSymbol));
    if (!plan->symbols)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate native symbols");
    plan->symbol_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[64]={0};
        manifest_format(ctx, where, sizeof(where), "native.symbol[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeSymbol *symbol = &plan->symbols[i];
        if (!native_validate_keys(ctx, plan, table, where, symbol_keys,
                                  sizeof(symbol_keys) / sizeof(symbol_keys[0])))
            return false;
        symbol->xray_name = native_dup_string(ctx, table, "xray");
        symbol->native_name = native_dup_string(ctx, table, "native");
        symbol->kind = native_symbol_kind(ctx, manifest_string(ctx, table, "kind"));
        symbol->calling_convention = native_dup_string(ctx, table, "calling_convention");
        symbol->unit_name = native_dup_string(ctx, table, "unit");
        if (!symbol->xray_name || !symbol->native_name || !symbol->kind ||
            !symbol->calling_convention || manifest_compare(ctx, symbol->calling_convention, "c") != 0 ||
            !symbol->unit_name)
            return native_fail(ctx, plan,
                               "E-NATIVE-SCHEMA: %s requires xray/native/kind/c calling "
                               "convention/unit",
                               where);
        symbol->unit = xr_native_package_find_unit_impl(ctx, plan, symbol->unit_name);
        if (!symbol->unit)
            return native_fail(ctx, plan, "E-NATIVE-SCHEMA: %s references unknown unit '%s'", where,
                               symbol->unit_name);
        for (int j = 0; manifest_work(ctx, 1) && (j < i); j++) {
            if (manifest_compare(ctx, plan->symbols[j].xray_name, symbol->xray_name) == 0)
                return native_fail(ctx, plan, "E-NATIVE-SCHEMA: duplicate Xray symbol '%s'",
                                   symbol->xray_name);
        }
        XrTomlValue *contract = manifest_get_type(ctx, table, "contract", XR_TOML_TABLE);
        if (!contract) {
            if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
                return native_fail(ctx, plan, "E-NATIVE-CONTRACT: shipping symbol '%s' has no contract",
                                   symbol->xray_name);
            continue;
        }
        char contract_where[80]={0};
        manifest_format(ctx, contract_where, sizeof(contract_where), "%s.contract", where);
        if (!native_parse_symbol_contract(ctx, plan, contract, &symbol->contract, contract_where))
            return false;
    }
    return true;
}

static bool native_parse_layouts(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const layout_keys[] = {"xray_type", "c_type", "header", "assert"};
    static const char *const assert_keys[] = {"size", "align", "fields"};
    XrTomlValue *array = manifest_get_type(ctx, native, "layout", XR_TOML_ARRAY);
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: native.layout cannot be empty");
    plan->layouts =
        (XrNativeLayoutAssertion *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeLayoutAssertion));
    if (!plan->layouts)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate native layouts");
    plan->layout_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[64]={0};
        manifest_format(ctx, where, sizeof(where), "native.layout[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeLayoutAssertion *layout = &plan->layouts[i];
        if (!native_validate_keys(ctx, plan, table, where, layout_keys,
                                  sizeof(layout_keys) / sizeof(layout_keys[0])))
            return false;
        layout->xray_type = native_dup_string(ctx, table, "xray_type");
        layout->c_type = native_dup_string(ctx, table, "c_type");
        const char *header = manifest_string(ctx, table, "header");
        if (!layout->xray_type || !native_valid_c_identifier(ctx, layout->c_type) || !header)
            return native_fail(ctx, plan, "E-NATIVE-LAYOUT: %s requires xray_type/c_type/header", where);
        char header_where[80]={0};
        manifest_format(ctx, header_where, sizeof(header_where), "%s.header", where);
        layout->header = native_resolve_existing_path(ctx, plan, header, header_where);
        if (!layout->header)
            return false;
        XrTomlValue *assertions = manifest_get_type(ctx, table, "assert", XR_TOML_TABLE);
        if (!native_validate_keys(ctx, plan, assertions, "native.layout.assert", assert_keys,
                                  sizeof(assert_keys) / sizeof(assert_keys[0])))
            return false;
        layout->assert_size = manifest_boolean(ctx, assertions, "size", false);
        layout->assert_align = manifest_boolean(ctx, assertions, "align", false);
        layout->assert_fields = manifest_boolean(ctx, assertions, "fields", false);
        if (!layout->assert_size || !layout->assert_align || !layout->assert_fields)
            return native_fail(ctx, plan, "E-NATIVE-LAYOUT: %s must assert size, align, and fields",
                               where);
    }
    return true;
}

static bool native_parse_capabilities(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const keys[] = {"type", "request", "attestation", "scope"};
    XrTomlValue *array = manifest_get_type(ctx, native, "capability", XR_TOML_ARRAY);
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: native.capability cannot be empty");
    plan->capabilities =
        (XrNativeCapability *) manifest_calloc(ctx, (size_t) count, sizeof(XrNativeCapability));
    if (!plan->capabilities)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate native capabilities");
    plan->capability_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[72]={0};
        manifest_format(ctx, where, sizeof(where), "native.capability[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeCapability *cap = &plan->capabilities[i];
        if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        cap->type_name = native_dup_string(ctx, table, "type");
        cap->request = native_dup_string(ctx, table, "request");
        cap->attestation = native_dup_string(ctx, table, "attestation");
        cap->scope = native_dup_string(ctx, table, "scope");
        if (!cap->type_name || !cap->request || !cap->attestation || !cap->scope)
            return native_fail(ctx, plan, "E-NATIVE-CAPABILITY: %s is incomplete", where);
        /* The package manifest is not an authority.  No external trust registry
         * has been installed in this compiler build, so every requested native
         * capability remains unverified and shipping builds fail closed. */
        cap->verified = false;
        if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
            return native_fail(ctx, plan,
                               "E-FFI-UNPROVEN-SHARE: capability for '%s' has no compiler/SDK "
                               "trust-registry attestation",
                               cap->type_name);
    }
    return true;
}

static bool native_valid_target_triple(ManifestContext *ctx, const char *triple) {
    if (!triple) return false;
    bool any=false;unsigned char previous=0;
    for (;;) {
        if (!manifest_work(ctx,1)) return false;
        unsigned char byte=(unsigned char)*triple++;
        if (!byte)return any;
        if (!isalnum(byte)&&byte!='_'&&byte!='-'&&byte!='.')return false;
        if (previous=='.'&&byte=='.')return false;
        previous=byte;any=true;
    }
}

static bool native_valid_c_identifier(ManifestContext *ctx, const char *name) {
    if (!name||!manifest_work(ctx,1))return false;
    unsigned char byte=(unsigned char)*name++;
    if (!isalpha(byte)&&byte!='_')return false;
    for (;;) {
        if (!manifest_work(ctx,1))return false;
        byte=(unsigned char)*name++;
        if (!byte)return true;
        if (!isalnum(byte)&&byte!='_')return false;
    }
}

static bool native_parse_targets(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const keys[] = {
        "profile", "visibility", "cpu_feature", "system_links", "vm",
    };
    XrTomlValue *targets = manifest_get_type(ctx, native, "target", XR_TOML_TABLE);
    if (!targets)
        return true;
    if (targets->as.table.count <= 0)
        return native_fail(ctx, plan, "E-NATIVE-SCHEMA: native.target cannot be empty");
    plan->targets = (XrNativeTargetPlan *) manifest_calloc(ctx, (size_t) targets->as.table.count,
                                                     sizeof(XrNativeTargetPlan));
    if (!plan->targets)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate native target plans");
    plan->target_count = (uint32_t) targets->as.table.count;
    for (int i = 0; manifest_work(ctx, 1) && (i < targets->as.table.count); i++) {
        const char *triple = targets->as.table.members[i].key;
        XrTomlValue *table = targets->as.table.members[i].value;
        XrNativeTargetPlan *target = &plan->targets[i];
        char where[192]={0};
        manifest_format(ctx, where, sizeof(where), "native.target.%s", triple ? triple : "?");
        if (!native_valid_target_triple(ctx, triple))
            return native_fail(ctx, plan, "E-NATIVE-TARGET: invalid canonical target triple '%s'",
                               triple ? triple : "");
        if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        target->triple = manifest_duplicate(ctx, triple);
        target->profile = native_dup_string(ctx, table, "profile");
        target->visibility = native_dup_string(ctx, table, "visibility");
        target->cpu_feature = native_dup_string(ctx, table, "cpu_feature");
        if (!target->triple || !target->profile ||
            (manifest_compare(ctx, target->profile, "debug") != 0 && manifest_compare(ctx, target->profile, "release") != 0 &&
             manifest_compare(ctx, target->profile, "freestanding") != 0))
            return native_fail(ctx, plan,
                               "E-NATIVE-TARGET: %s.profile must be debug, release, or "
                               "freestanding",
                               where);
        if (target->visibility && manifest_compare(ctx, target->visibility, "hidden") != 0 &&
            manifest_compare(ctx, target->visibility, "default") != 0)
            return native_fail(ctx, plan, "E-NATIVE-TARGET: %s.visibility must be hidden or default",
                               where);
        if (target->cpu_feature && manifest_compare(ctx, target->cpu_feature, "baseline") != 0)
            return native_fail(ctx,
                plan, "E-NATIVE-TARGET: %s.cpu_feature must be the sealed value baseline", where);
        if (!native_parse_string_array(ctx, plan, table, "system_links", false, &target->system_links,
                                       &target->system_link_count, where))
            return false;
        const char *vm = manifest_string(ctx, table, "vm");
        if (vm) {
            if (manifest_compare(ctx, vm, "verified-dynamic") == 0)
                target->vm_policy = XR_NATIVE_VM_VERIFIED_DYNAMIC;
            else if (manifest_compare(ctx, vm, "unsupported") == 0)
                target->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
            else
                return native_fail(ctx, plan,
                                   "E-NATIVE-TARGET: %s.vm must be verified-dynamic or "
                                   "unsupported",
                                   where);
        } else {
            target->vm_policy = plan->vm_policy;
        }
    }
    return true;
}

static bool native_parse_exports(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "symbol", "visibility", "header", "abi"};
    XrTomlValue *group = manifest_get_type(ctx, root, "export", XR_TOML_TABLE);
    XrTomlValue *array = group ? manifest_get_type(ctx, group, "c", XR_TOML_ARRAY) : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-EXPORT-SCHEMA: export.c cannot be empty");
    plan->exports = (XrCExportPlan *) manifest_calloc(ctx, (size_t) count, sizeof(XrCExportPlan));
    if (!plan->exports)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate C export plans");
    plan->export_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[64]={0};
        manifest_format(ctx, where, sizeof(where), "export.c[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrCExportPlan *item = &plan->exports[i];
        if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(ctx, table, "xray");
        item->symbol = native_dup_string(ctx, table, "symbol");
        item->visibility = native_dup_string(ctx, table, "visibility");
        item->abi = native_dup_string(ctx, table, "abi");
        item->header = manifest_boolean(ctx, table, "header", false);
        if (!item->xray_name || !item->xray_name[0] || !native_valid_c_identifier(ctx, item->symbol))
            return native_fail(ctx, plan, "E-EXPORT-SCHEMA: %s requires xray and C identifier symbol",
                               where);
        if (item->visibility && manifest_compare(ctx, item->visibility, "default") != 0 &&
            manifest_compare(ctx, item->visibility, "hidden") != 0)
            return native_fail(ctx, plan, "E-EXPORT-SCHEMA: %s.visibility must be default or hidden",
                               where);
        if (item->abi && manifest_compare(ctx, item->abi, "native") != 0 &&
            manifest_compare(ctx, item->abi, "hosted-vm-v1") != 0)
            return native_fail(ctx, plan,
                               "E-EXPORT-SCHEMA: %s.abi must be native or hosted-vm-v1",
                               where);
        for (int j = 0; manifest_work(ctx, 1) && (j < i); j++) {
            if (manifest_compare(ctx, plan->exports[j].xray_name, item->xray_name) == 0 ||
                manifest_compare(ctx, plan->exports[j].symbol, item->symbol) == 0)
                return native_fail(ctx, plan, "E-EXPORT-SCHEMA: duplicate export '%s'", item->xray_name);
        }
    }
    return true;
}

static bool native_parse_link_symbols(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "section", "used", "weak"};
    XrTomlValue *group = manifest_get_type(ctx, root, "link", XR_TOML_TABLE);
    XrTomlValue *array = group ? manifest_get_type(ctx, group, "symbol", XR_TOML_ARRAY) : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-LINK-SCHEMA: link.symbol cannot be empty");
    plan->link_symbols = (XrLinkSymbolPlan *) manifest_calloc(ctx, (size_t) count, sizeof(XrLinkSymbolPlan));
    if (!plan->link_symbols)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate link symbol plans");
    plan->link_symbol_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[64]={0};
        manifest_format(ctx, where, sizeof(where), "link.symbol[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrLinkSymbolPlan *item = &plan->link_symbols[i];
        if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(ctx, table, "xray");
        item->section = native_dup_string(ctx, table, "section");
        item->used = manifest_boolean(ctx, table, "used", false);
        item->weak = manifest_boolean(ctx, table, "weak", false);
        if (!item->xray_name || !item->xray_name[0])
            return native_fail(ctx, plan, "E-LINK-SCHEMA: %s requires non-empty xray", where);
        if (item->section && !item->section[0])
            return native_fail(ctx, plan, "E-LINK-SCHEMA: %s.section cannot be empty", where);
        if (!item->section && !item->used && !item->weak)
            return native_fail(ctx, plan, "E-LINK-SCHEMA: %s requires section, used=true, or weak=true",
                               where);
        for (int j = 0; manifest_work(ctx, 1) && (j < i); j++)
            if (manifest_compare(ctx, plan->link_symbols[j].xray_name, item->xray_name) == 0)
                return native_fail(ctx, plan, "E-LINK-SCHEMA: duplicate link symbol '%s'",
                                   item->xray_name);
    }
    return true;
}

static XrFreestandingEntryKind native_entry_kind(ManifestContext *ctx, const char *kind) {
    if (!kind)
        return 0;
    if (manifest_compare(ctx, kind, "start") == 0)
        return XR_FREESTANDING_ENTRY_START;
    if (manifest_compare(ctx, kind, "interrupt") == 0)
        return XR_FREESTANDING_ENTRY_INTERRUPT;
    if (manifest_compare(ctx, kind, "naked-stub") == 0)
        return XR_FREESTANDING_ENTRY_NAKED_STUB;
    return 0;
}

static bool native_parse_entries(ManifestContext *ctx, XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "symbol", "kind", "abi", "section", "stub"};
    XrTomlValue *group = manifest_get_type(ctx, root, "freestanding", XR_TOML_TABLE);
    XrTomlValue *array = group ? manifest_get_type(ctx, group, "entry", XR_TOML_ARRAY) : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(ctx, plan, "E-ENTRY-SCHEMA: freestanding.entry cannot be empty");
    plan->entries =
        (XrFreestandingEntryPlan *) manifest_calloc(ctx, (size_t) count, sizeof(XrFreestandingEntryPlan));
    if (!plan->entries)
        return native_fail(ctx, plan, "E-NATIVE-RESOURCE: cannot allocate freestanding entry plans");
    plan->entry_count = (uint32_t) count;
    for (int i = 0; manifest_work(ctx, 1) && (i < count); i++) {
        char where[72]={0};
        manifest_format(ctx, where, sizeof(where), "freestanding.entry[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrFreestandingEntryPlan *item = &plan->entries[i];
        if (!native_validate_keys(ctx, plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(ctx, table, "xray");
        item->symbol = native_dup_string(ctx, table, "symbol");
        item->kind = native_entry_kind(ctx, manifest_string(ctx, table, "kind"));
        item->abi = native_dup_string(ctx, table, "abi");
        item->section = native_dup_string(ctx, table, "section");
        const char *stub = manifest_string(ctx, table, "stub");
        if (stub) {
            char stub_where[96]={0};
            manifest_format(ctx, stub_where, sizeof(stub_where), "%s.stub", where);
            item->stub = native_resolve_existing_path(ctx, plan, stub, stub_where);
            if (!item->stub)
                return false;
        }
        if (!item->xray_name || !item->xray_name[0] || !native_valid_c_identifier(ctx, item->symbol) ||
            !item->kind || !item->section || !item->section[0])
            return native_fail(ctx, plan, "E-ENTRY-SCHEMA: %s requires xray/symbol/kind/section", where);
        if (item->kind == XR_FREESTANDING_ENTRY_INTERRUPT && (!item->abi || !item->abi[0]))
            return native_fail(ctx, plan, "E-ENTRY-SCHEMA: %s interrupt requires abi", where);
        if ((item->kind == XR_FREESTANDING_ENTRY_NAKED_STUB ||
             item->kind == XR_FREESTANDING_ENTRY_INTERRUPT) &&
            !item->stub)
            return native_fail(ctx, plan, "E-ENTRY-SCHEMA: %s requires an audited stub", where);
        for (int j = 0; manifest_work(ctx, 1) && (j < i); j++)
            if (manifest_compare(ctx, plan->entries[j].xray_name, item->xray_name) == 0 ||
                manifest_compare(ctx, plan->entries[j].symbol, item->symbol) == 0)
                return native_fail(ctx, plan, "E-ENTRY-SCHEMA: duplicate entry '%s'", item->xray_name);
    }
    return true;
}

static void native_refresh_plan_fingerprint(ManifestContext *ctx, XrNativePackagePlan *plan) {
    uint64_t fingerprint = XR_NATIVE_FNV_OFFSET;
    fingerprint = native_hash_text(ctx, fingerprint, plan->name);
    fingerprint = native_hash_text(ctx, fingerprint, plan->version);
    fingerprint = native_hash_text(ctx, fingerprint, plan->license);
    fingerprint = native_hash_text(ctx, fingerprint, plan->source);
    fingerprint = native_hash_bytes(ctx, fingerprint, &plan->audit_mode, sizeof(plan->audit_mode));
    fingerprint = native_hash_bytes(ctx, fingerprint, &plan->vm_policy, sizeof(plan->vm_policy));
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->unit_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->units[i].name);
        fingerprint = native_hash_bytes(ctx, fingerprint, &plan->units[i].fingerprint,
                                        sizeof(plan->units[i].fingerprint));
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->symbol_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->symbols[i].xray_name);
        fingerprint = native_hash_text(ctx, fingerprint, plan->symbols[i].native_name);
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->target_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->targets[i].triple);
        fingerprint = native_hash_text(ctx, fingerprint, plan->targets[i].profile);
        fingerprint = native_hash_text(ctx, fingerprint, plan->targets[i].visibility);
        fingerprint = native_hash_text(ctx, fingerprint, plan->targets[i].cpu_feature);
        for (uint32_t j = 0; manifest_work(ctx, 1) && (j < plan->targets[i].system_link_count); j++)
            fingerprint = native_hash_text(ctx, fingerprint, plan->targets[i].system_links[j]);
        fingerprint = native_hash_bytes(ctx, fingerprint, &plan->targets[i].vm_policy,
                                        sizeof(plan->targets[i].vm_policy));
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->exports[i].xray_name);
        fingerprint = native_hash_text(ctx, fingerprint, plan->exports[i].symbol);
        fingerprint = native_hash_text(ctx, fingerprint, plan->exports[i].visibility);
        fingerprint = native_hash_text(ctx, fingerprint, plan->exports[i].abi);
        fingerprint = native_hash_bytes(ctx, fingerprint, &plan->exports[i].header,
                                        sizeof(plan->exports[i].header));
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->link_symbol_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->link_symbols[i].xray_name);
        fingerprint = native_hash_text(ctx, fingerprint, plan->link_symbols[i].section);
        fingerprint = native_hash_bytes(ctx, fingerprint, &plan->link_symbols[i].used,
                                        sizeof(plan->link_symbols[i].used));
        fingerprint = native_hash_bytes(ctx, fingerprint, &plan->link_symbols[i].weak,
                                        sizeof(plan->link_symbols[i].weak));
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->entry_count); i++) {
        fingerprint = native_hash_text(ctx, fingerprint, plan->entries[i].xray_name);
        fingerprint = native_hash_text(ctx, fingerprint, plan->entries[i].symbol);
        fingerprint =
            native_hash_bytes(ctx, fingerprint, &plan->entries[i].kind, sizeof(plan->entries[i].kind));
        fingerprint = native_hash_text(ctx, fingerprint, plan->entries[i].abi);
        fingerprint = native_hash_text(ctx, fingerprint, plan->entries[i].section);
        fingerprint = native_hash_text(ctx, fingerprint, plan->entries[i].stub);
    }
    plan->fingerprint = fingerprint ? fingerprint : 1;
}

static XrNativePackagePlan *xr_native_package_plan_parse_impl(ManifestContext *ctx, XrTomlValue *toml_root,
                                                  const char *project_root) {
    static const char *const native_keys[] = {
        "name", "version", "license", "source",     "audit_mode", "vm",
        "unit", "symbol",  "layout",  "capability", "target",
    };
    if (!toml_root || !project_root)
        return NULL;
    XrTomlValue *native = manifest_get_type(ctx, toml_root, "native", XR_TOML_TABLE);
    bool has_export = manifest_get_type(ctx, toml_root, "export", XR_TOML_TABLE) != NULL;
    bool has_link = manifest_get_type(ctx, toml_root, "link", XR_TOML_TABLE) != NULL;
    bool has_entry = manifest_get_type(ctx, toml_root, "freestanding", XR_TOML_TABLE) != NULL;
    if (!native && !has_export && !has_link && !has_entry)
        return NULL;
    XrNativePackagePlan *plan = (XrNativePackagePlan *) manifest_calloc(ctx, 1, sizeof(XrNativePackagePlan));
    if (!plan)
        return NULL;
    plan->root = manifest_realpath(ctx, project_root);
    if (!plan->root) return plan;
    plan->valid = true;
    if (native) {
        if (!native_validate_keys(ctx, plan, native, "native", native_keys,
                                  sizeof(native_keys) / sizeof(native_keys[0])))
            return plan;
        plan->name = native_dup_string(ctx, native, "name");
        plan->version = native_dup_string(ctx, native, "version");
        plan->license = native_dup_string(ctx, native, "license");
        plan->source = native_dup_string(ctx, native, "source");
        if (!plan->name || !plan->version || !plan->license || !plan->source ||
            !native_parse_audit(ctx, plan, manifest_string(ctx, native, "audit_mode")) ||
            !native_parse_vm_policy(ctx, plan, manifest_string(ctx, native, "vm"))) {
            if (!plan->error)
                native_fail(ctx, plan, "E-NATIVE-SCHEMA: native requires "
                                  "name/version/license/source/audit_mode/vm");
            return plan;
        }
        if (!native_parse_units(ctx, plan, native) || !native_parse_symbols(ctx, plan, native) ||
            !native_parse_layouts(ctx, plan, native) || !native_parse_capabilities(ctx, plan, native) ||
            !native_parse_targets(ctx, plan, native))
            return plan;
    } else {
        plan->name = manifest_duplicate(ctx, "project");
        plan->version = manifest_duplicate(ctx, "0");
        plan->license = manifest_duplicate(ctx, "project");
        plan->source = manifest_duplicate(ctx, "project");
        plan->audit_mode = XR_NATIVE_AUDIT_NONE;
        plan->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
    }
    if (!native_parse_exports(ctx, plan, toml_root) || !native_parse_link_symbols(ctx, plan, toml_root) ||
        !native_parse_entries(ctx, plan, toml_root))
        return plan;
    if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING &&
        (plan->unit_count == 0 || (plan->symbol_count == 0 && plan->entry_count == 0))) {
        native_fail(ctx, plan, "E-NATIVE-SCHEMA: shipping native package requires units and symbols or "
                          "freestanding entries");
        return plan;
    }
    native_refresh_plan_fingerprint(ctx, plan);
    return plan;
}

static void native_symbol_contract_free(XrNativeSymbolContract *contract) {
    if (!contract)
        return;
    manifest_free(contract->params);
    manifest_free(contract->result.validity);
    manifest_free(contract->result.drop_function);
    native_free_string_array(contract->effects, contract->effect_count);
    manifest_free(contract->callbacks);
    manifest_free(contract->failure);
    manifest_free(contract->allocation);
    manifest_free(contract->blocking);
    manifest_free(contract->suspend);
    manifest_free(contract->io);
    manifest_free(contract->sync);
    manifest_free(contract->panic);
    manifest_free(contract->error);
}

XR_FUNC void xr_native_package_plan_free_owned(XrNativePackagePlan *plan) {
    if (!plan)
        return;
    manifest_free(plan->root);
    manifest_free(plan->name);
    manifest_free(plan->version);
    manifest_free(plan->license);
    manifest_free(plan->source);
    manifest_free(plan->error);
    for (uint32_t i = 0; i < plan->unit_count; i++) {
        XrNativeUnit *unit = &plan->units[i];
        manifest_free(unit->name);
        native_free_string_array(unit->sources, unit->source_count);
        native_free_string_array(unit->source_relpaths, unit->source_count);
        native_free_string_array(unit->source_hashes, unit->source_count);
        native_free_string_array(unit->include_dirs, unit->include_dir_count);
        native_free_string_array(unit->defines, unit->define_count);
        native_free_string_array(unit->system_links, unit->system_link_count);
        manifest_free(unit->language_standard);
        manifest_free(unit->optimization);
        manifest_free(unit->visibility);
        manifest_free(unit->warning_policy);
        manifest_free(unit->cpu_feature);
        manifest_free(unit->output);
        manifest_free(unit->purpose);
    }
    manifest_free(plan->units);
    for (uint32_t i = 0; i < plan->symbol_count; i++) {
        XrNativeSymbol *symbol = &plan->symbols[i];
        manifest_free(symbol->xray_name);
        manifest_free(symbol->native_name);
        manifest_free(symbol->calling_convention);
        manifest_free(symbol->unit_name);
        native_symbol_contract_free(&symbol->contract);
    }
    manifest_free(plan->symbols);
    for (uint32_t i = 0; i < plan->layout_count; i++) {
        manifest_free(plan->layouts[i].xray_type);
        manifest_free(plan->layouts[i].c_type);
        manifest_free(plan->layouts[i].header);
        native_free_string_array(plan->layouts[i].field_names, plan->layouts[i].field_count);
        manifest_free(plan->layouts[i].field_offsets);
    }
    manifest_free(plan->layouts);
    for (uint32_t i = 0; i < plan->capability_count; i++) {
        manifest_free(plan->capabilities[i].type_name);
        manifest_free(plan->capabilities[i].request);
        manifest_free(plan->capabilities[i].attestation);
        manifest_free(plan->capabilities[i].scope);
    }
    manifest_free(plan->capabilities);
    for (uint32_t i = 0; i < plan->target_count; i++) {
        manifest_free(plan->targets[i].triple);
        manifest_free(plan->targets[i].profile);
        manifest_free(plan->targets[i].visibility);
        manifest_free(plan->targets[i].cpu_feature);
        native_free_string_array(plan->targets[i].system_links, plan->targets[i].system_link_count);
    }
    manifest_free(plan->targets);
    for (uint32_t i = 0; i < plan->export_count; i++) {
        manifest_free(plan->exports[i].xray_name);
        manifest_free(plan->exports[i].symbol);
        manifest_free(plan->exports[i].visibility);
        manifest_free(plan->exports[i].abi);
    }
    manifest_free(plan->exports);
    for (uint32_t i = 0; i < plan->link_symbol_count; i++) {
        manifest_free(plan->link_symbols[i].xray_name);
        manifest_free(plan->link_symbols[i].section);
    }
    manifest_free(plan->link_symbols);
    for (uint32_t i = 0; i < plan->entry_count; i++) {
        manifest_free(plan->entries[i].xray_name);
        manifest_free(plan->entries[i].symbol);
        manifest_free(plan->entries[i].abi);
        manifest_free(plan->entries[i].section);
        manifest_free(plan->entries[i].stub);
    }
    manifest_free(plan->entries);
    manifest_free(plan);
}

static const XrNativeUnit *xr_native_package_find_unit_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, const char *name) {
    if (!plan || !name)
        return NULL;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->unit_count); i++) {
        if (plan->units[i].name && manifest_compare(ctx, plan->units[i].name, name) == 0)
            return &plan->units[i];
    }
    return NULL;
}

static const char *native_final_component(ManifestContext *ctx, const char *name) {
    const char *dot = name ? manifest_find(ctx, name, '.', true) : NULL;
    return dot ? dot + 1 : name;
}

static const XrNativeSymbol *xr_native_package_find_symbol_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                    const char *xray_name) {
    const XrNativeSymbol *short_match = NULL;
    if (!plan || !plan->valid || !xray_name)
        return NULL;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->symbol_count); i++) {
        const XrNativeSymbol *symbol = &plan->symbols[i];
        if (manifest_compare(ctx, symbol->xray_name, xray_name) == 0)
            return symbol;
        if (manifest_compare(ctx, native_final_component(ctx, symbol->xray_name), xray_name) == 0) {
            if (short_match)
                return NULL;
            short_match = symbol;
        }
    }
    return short_match;
}

static bool native_plan_name_matches(ManifestContext *ctx, const char *declared, const char *requested) {
    if (!declared || !requested)
        return false;
    if (manifest_compare(ctx, declared, requested) == 0)
        return true;
    const char *dot = manifest_find(ctx, declared, '.', true);
    return dot && manifest_compare(ctx, dot + 1, requested) == 0;
}

static const XrCExportPlan *xr_native_package_find_export_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                   const char *xray_name) {
    const XrCExportPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++) {
        if (!native_plan_name_matches(ctx, plan->exports[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->exports[i];
    }
    return found;
}

static bool native_csv_has_symbol(ManifestContext *ctx, const char *csv, const char *symbol) {
    if (!csv || !csv[0] || !symbol)
        return false;
    size_t symbol_len = manifest_length(ctx, symbol);
    const char *item = csv;
    while (manifest_work(ctx, 1) && (*item)) {
        const char *comma = manifest_find(ctx, item, ',', false);
        size_t item_len = comma ? (size_t) (comma - item) : manifest_length(ctx, item);
        if (item_len == symbol_len && manifest_compare_bytes(ctx, item, symbol, item_len) == 0)
            return true;
        if (!comma)
            break;
        item = comma + 1;
    }
    return false;
}

static bool native_validate_export_excludes(ManifestContext *ctx, const XrNativePackagePlan *plan, const char *csv,
                                            char *error, size_t error_size) {
    if (!csv || !csv[0])
        return true;
    const char *item = csv;
    while (manifest_work(ctx, 1) && (true)) {
        const char *comma = manifest_find(ctx, item, ',', false);
        size_t item_len = comma ? (size_t) (comma - item) : manifest_length(ctx, item);
        if (item_len == 0) {
            if (error && error_size)
                manifest_format(ctx, error, error_size, "--c-export-exclude contains an empty symbol");
            return false;
        }
        char *symbol = (char *) manifest_alloc(ctx, item_len + 1);
        if (!symbol) {
            if (error && error_size)
                manifest_format(ctx, error, error_size, "cannot allocate C export filter");
            return false;
        }
        manifest_copy(ctx, symbol, item, item_len);
        symbol[item_len] = '\0';
        bool valid = native_valid_c_identifier(ctx, symbol);
        bool found = false;
        if (valid) {
            for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++) {
                if (manifest_compare(ctx, plan->exports[i].symbol, symbol) == 0) {
                    found = true;
                    break;
                }
            }
        }
        if (!valid && error && error_size)
            manifest_format(ctx, error, error_size, "invalid C export symbol in --c-export-exclude: %s",
                     symbol);
        else if (!found && error && error_size)
            manifest_format(ctx, error, error_size, "unknown C export symbol in --c-export-exclude: %s",
                     symbol);
        manifest_free(symbol);
        if (!valid || !found)
            return false;
        if (!comma)
            break;
        item = comma + 1;
    }
    return true;
}

static bool xr_native_package_configure_c_exports_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *public_prefix,
                                           const char *exclude_csv, char *error,
                                           size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!plan || !plan->valid) {
        if (error && error_size)
            manifest_format(ctx, error, error_size, "C export shaping requires a valid project manifest");
        return false;
    }
    if (public_prefix && public_prefix[0] && !native_valid_c_identifier(ctx, public_prefix)) {
        if (error && error_size)
            manifest_format(ctx, error, error_size, "invalid --c-export-prefix C identifier: %s",
                     public_prefix);
        return false;
    }
    if (!native_validate_export_excludes(ctx, plan, exclude_csv, error, error_size))
        return false;

    char **renamed = NULL;
    if (public_prefix && public_prefix[0] && plan->export_count > 0) {
        renamed = (char **) manifest_calloc(ctx, plan->export_count, sizeof(char *));
        if (!renamed) {
            if (error && error_size)
                manifest_format(ctx, error, error_size, "cannot allocate prefixed C exports");
            return false;
        }
        size_t prefix_len = manifest_length(ctx, public_prefix);
        for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++) {
            XrCExportPlan *item = &plan->exports[i];
            if (item->visibility && manifest_compare(ctx, item->visibility, "hidden") == 0)
                continue;
            size_t symbol_len = manifest_length(ctx, item->symbol);
            if (prefix_len>SIZE_MAX-1 || symbol_len>SIZE_MAX-prefix_len-1) manifest_status(ctx,XR_MANIFEST_BUDGET);
            renamed[i] = (char *) manifest_alloc(ctx, prefix_len + symbol_len + 1);
            if (!renamed[i]) {
                for (uint32_t j = 0; j < i; j++)
                    manifest_free(renamed[j]);
                manifest_free(renamed);
                if (error && error_size)
                    manifest_format(ctx, error, error_size, "cannot allocate prefixed C export symbol");
                return false;
            }
            manifest_copy(ctx, renamed[i], public_prefix, prefix_len);
            manifest_copy(ctx, renamed[i] + prefix_len, item->symbol, symbol_len + 1);
        }
    }

    uint32_t write = 0;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++) {
        XrCExportPlan *item = &plan->exports[i];
        bool excluded=native_csv_has_symbol(ctx, exclude_csv, item->symbol);
        if (ctx->status!=XR_MANIFEST_OK) break;
        if (excluded) {
            manifest_free(item->xray_name);
            manifest_free(item->symbol);
            manifest_free(item->visibility);
            manifest_free(item->abi);
            if (renamed) { manifest_free(renamed[i]);renamed[i]=NULL; }
            memset(item,0,sizeof(*item));
            continue;
        }
        if (renamed && renamed[i]) {
            manifest_free(item->symbol);
            item->symbol = renamed[i];renamed[i]=NULL;
        }
        if (write != i) { plan->exports[write] = *item;memset(item,0,sizeof(*item)); }
        write++;
    }
    if (renamed) for (uint32_t i=0;i<plan->export_count;++i) manifest_free(renamed[i]);
    manifest_free(renamed);
    if (ctx->status!=XR_MANIFEST_OK) return false;
    plan->export_count = write;
    native_refresh_plan_fingerprint(ctx, plan);
    return true;
}

static const XrLinkSymbolPlan *xr_native_package_find_link_symbol_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                           const char *xray_name) {
    const XrLinkSymbolPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->link_symbol_count); i++) {
        if (!native_plan_name_matches(ctx, plan->link_symbols[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->link_symbols[i];
    }
    return found;
}

static const XrFreestandingEntryPlan *xr_native_package_find_entry_impl(ManifestContext *ctx, const XrNativePackagePlan *plan,
                                                            const char *xray_name) {
    const XrFreestandingEntryPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->entry_count); i++) {
        if (!native_plan_name_matches(ctx, plan->entries[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->entries[i];
    }
    return found;
}

XR_FUNC const char *xr_native_symbol_library(const XrNativeSymbol *symbol) {
    if (!symbol || !symbol->unit)
        return NULL;
    if (symbol->unit->output && symbol->unit->output[0])
        return symbol->unit->output;
    if (symbol->unit->kind == XR_NATIVE_UNIT_PLATFORM && symbol->unit->system_link_count == 1)
        return symbol->unit->system_links[0];
    if ((symbol->unit->kind == XR_NATIVE_UNIT_OBJECT ||
         symbol->unit->kind == XR_NATIVE_UNIT_STATIC_LIBRARY ||
         symbol->unit->kind == XR_NATIVE_UNIT_DYNAMIC_LIBRARY) &&
        symbol->unit->source_count == 1)
        return symbol->unit->sources[0];
    return NULL;
}

/* Does `assertion` name `xray_type`, either fully qualified or by final path
 * component? */
static bool native_layout_assertion_names(ManifestContext *ctx, const XrNativeLayoutAssertion *assertion,
                                          const char *xray_type) {
    if (!assertion || !assertion->xray_type || !xray_type)
        return false;
    const char *short_name = manifest_find(ctx, assertion->xray_type, '.', true);
    short_name = short_name ? short_name + 1 : assertion->xray_type;
    return manifest_compare(ctx, assertion->xray_type, xray_type) == 0 || manifest_compare(ctx, short_name, xray_type) == 0;
}

static void xr_native_package_note_layout_subject_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *xray_type) {
    if (!plan || !xray_type)
        return;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->layout_count); i++) {
        if (native_layout_assertion_names(ctx, &plan->layouts[i], xray_type))
            plan->layouts[i].declared = true;
    }
}

static bool xr_native_package_resolve_layout_impl(ManifestContext *ctx, XrNativePackagePlan *plan, const char *xray_type,
                                      const XrAggregateLayout *layout) {
    bool matched = false;
    if (!plan || !xray_type || !layout)
        return false;
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->layout_count); i++) {
        XrNativeLayoutAssertion *assertion = &plan->layouts[i];
        if (!native_layout_assertion_names(ctx, assertion, xray_type))
            continue;
        assertion->declared = true;
        native_free_string_array(assertion->field_names, assertion->field_count);
        manifest_free(assertion->field_offsets);
        assertion->field_names = NULL;
        assertion->field_offsets = NULL;
        assertion->field_count = layout->field_count;
        if (layout->field_count > 0) {
            assertion->field_names =
                (char **) manifest_calloc(ctx, (size_t) layout->field_count, sizeof(char *));
            assertion->field_offsets =
                (uint32_t *) manifest_calloc(ctx, (size_t) layout->field_count, sizeof(uint32_t));
            if (!assertion->field_names || !assertion->field_offsets) {
                native_free_string_array(assertion->field_names, assertion->field_count);
                manifest_free(assertion->field_offsets);
                assertion->field_names = NULL;
                assertion->field_offsets = NULL;
                assertion->field_count = 0;
                assertion->resolved = false;
                return false;
            }
            for (uint32_t fi = 0; manifest_work(ctx, 1) && (fi < layout->field_count); fi++) {
                assertion->field_names[fi] = manifest_duplicate(ctx,
                    layout->field_names && layout->field_names[fi] ? layout->field_names[fi] : "");
                assertion->field_offsets[fi] = layout->fields[fi].offset;
                if (!assertion->field_names[fi])
                    return false;
            }
        }
        assertion->expected_size = layout->total_size;
        assertion->expected_align = layout->alignment;
        assertion->resolved = true;
        matched = true;
    }
    return matched;
}

static bool xr_native_package_validate_symbol_arity_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, const char *xray_name,
                                             uint32_t arity, char *errbuf, size_t errbuf_len) {
    const XrNativeSymbol *symbol = xr_native_package_find_symbol_impl(ctx, plan, xray_name);
    if (!symbol) {
        /* Export/link/entry-only manifests do not seal the process C symbol
         * namespace. A present [native] table does: exploratory entries may
         * omit detailed contracts, but every mapped name is still explicit. */
        if (!plan || plan->audit_mode == XR_NATIVE_AUDIT_NONE)
            return true;
        if (errbuf && errbuf_len)
            manifest_format(ctx, errbuf, errbuf_len,
                     "E-NATIVE-UNDECLARED-SYMBOL: extern '%s' is absent from NativePackagePlan",
                     xray_name ? xray_name : "?");
        return false;
    }
    if (!symbol->contract.complete)
        return true;
    if (symbol->contract.param_count != arity) {
        if (errbuf && errbuf_len)
            manifest_format(ctx, errbuf, errbuf_len,
                     "E-NATIVE-CONTRACT: extern '%s' has arity %u but its contract describes %u",
                     xray_name ? xray_name : "?", arity, symbol->contract.param_count);
        return false;
    }
    return true;
}

XR_FUNC const char *xr_native_audit_mode_name(XrNativeAuditMode mode) {
    switch (mode) {
        case XR_NATIVE_AUDIT_EXPLORATORY:
            return "exploratory";
        case XR_NATIVE_AUDIT_SHIPPING:
            return "shipping";
        default:
            return "none";
    }
}

XR_FUNC const char *xr_native_unit_kind_name(XrNativeUnitKind kind) {
    switch (kind) {
        case XR_NATIVE_UNIT_C:
            return "c";
        case XR_NATIVE_UNIT_ASM:
            return "asm";
        case XR_NATIVE_UNIT_OBJECT:
            return "object";
        case XR_NATIVE_UNIT_STATIC_LIBRARY:
            return "static-library";
        case XR_NATIVE_UNIT_DYNAMIC_LIBRARY:
            return "dynamic-library";
        case XR_NATIVE_UNIT_PLATFORM:
            return "platform";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xr_native_param_access_name(XrNativeParamAccess access) {
    switch (access) {
        case XR_NATIVE_ACCESS_NONE:
            return "none";
        case XR_NATIVE_ACCESS_READ:
            return "read";
        case XR_NATIVE_ACCESS_WRITE:
            return "write";
        case XR_NATIVE_ACCESS_READWRITE:
            return "readwrite";
        default:
            return "unknown";
    }
}

static void xr_native_package_explain_impl(ManifestContext *ctx, const XrNativePackagePlan *plan, FILE *out) {
    if (!out)
        out = stdout;
    if (!plan) {
        manifest_print(ctx, out, "native-plan: none\n");
        return;
    }
    manifest_print(ctx, out, "native-plan package=%s version=%s audit=%s vm=%s fingerprint=%016llx valid=%s\n",
            plan->name ? plan->name : "?", plan->version ? plan->version : "?",
            xr_native_audit_mode_name(plan->audit_mode),
            plan->vm_policy == XR_NATIVE_VM_VERIFIED_DYNAMIC ? "verified-dynamic" : "unsupported",
            (unsigned long long) plan->fingerprint, plan->valid ? "yes" : "no");
    if (!plan->valid) {
        manifest_print(ctx, out, "error: %s\n", plan->error ? plan->error : "invalid native plan");
        return;
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->unit_count); i++) {
        const XrNativeUnit *unit = &plan->units[i];
        manifest_print(ctx, out, "unit %s kind=%s fingerprint=%016llx purpose=%s output=%s\n", unit->name,
                xr_native_unit_kind_name(unit->kind), (unsigned long long) unit->fingerprint,
                unit->purpose ? unit->purpose : "?", unit->output ? unit->output : "none");
        for (uint32_t si = 0; manifest_work(ctx, 1) && (si < unit->source_count); si++)
            manifest_print(ctx, out, "  source %s sha256=%s\n", unit->source_relpaths[si],
                    unit->source_hashes[si]);
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->symbol_count); i++) {
        const XrNativeSymbol *symbol = &plan->symbols[i];
        manifest_print(ctx, out, "symbol %s -> %s unit=%s contract=%s params=%u\n", symbol->xray_name,
                symbol->native_name, symbol->unit_name,
                symbol->contract.complete ? "complete" : "exploratory",
                symbol->contract.param_count);
        for (uint32_t pi = 0; manifest_work(ctx, 1) && (pi < symbol->contract.param_count); pi++) {
            const XrNativeParamContract *param = &symbol->contract.params[pi];
            manifest_print(ctx, out,
                    "  param %u access=%s nullable=%s length_from=%d escape=%u ownership=%u "
                    "rebind=%u relocate=%u shorten=%u invalidate=%u\n",
                    pi, xr_native_param_access_name(param->access), param->nullable ? "yes" : "no",
                    param->length_from, (unsigned) param->escape, (unsigned) param->ownership,
                    param->descriptor_rebind, param->may_relocate, param->may_shorten,
                    param->invalidates_views);
        }
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->target_count); i++) {
        const XrNativeTargetPlan *target = &plan->targets[i];
        manifest_print(ctx, out, "target %s profile=%s visibility=%s vm=%s cpu=%s\n", target->triple,
                target->profile, target->visibility ? target->visibility : "unit-default",
                target->vm_policy == XR_NATIVE_VM_VERIFIED_DYNAMIC ? "verified-dynamic"
                                                                   : "unsupported",
                target->cpu_feature ? target->cpu_feature : "baseline");
    }
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->export_count); i++)
        manifest_print(ctx, out, "export.c %s -> %s visibility=%s abi=%s header=%s\n",
                plan->exports[i].xray_name, plan->exports[i].symbol,
                plan->exports[i].visibility ? plan->exports[i].visibility : "default",
                plan->exports[i].abi ? plan->exports[i].abi : "native",
                plan->exports[i].header ? "yes" : "no");
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->link_symbol_count); i++)
        manifest_print(ctx, out, "link.symbol %s section=%s used=%s weak=%s\n", plan->link_symbols[i].xray_name,
                plan->link_symbols[i].section ? plan->link_symbols[i].section : "none",
                plan->link_symbols[i].used ? "yes" : "no",
                plan->link_symbols[i].weak ? "yes" : "no");
    for (uint32_t i = 0; manifest_work(ctx, 1) && (i < plan->entry_count); i++)
        manifest_print(ctx, out, "freestanding.entry %s -> %s kind=%u abi=%s section=%s stub=%s\n",
                plan->entries[i].xray_name, plan->entries[i].symbol,
                (unsigned) plan->entries[i].kind,
                plan->entries[i].abi ? plan->entries[i].abi : "none", plan->entries[i].section,
                plan->entries[i].stub ? plan->entries[i].stub : "none");
}

XR_FUNC bool xr_native_package_plan_uses_policy(const XrNativePackagePlan *plan, const XrOsIoPolicy *policy) {
    return manifest_uses_policy(plan,policy);
}
XR_FUNC XrManifestStatus xr_native_package_plan_parse_owned(const XrOsIoPolicy *policy,
    XrTomlValue *document, const char *absolute_root, XrNativePackagePlan **output,
    XrManifestDiagnostic *diagnostic) {
    if (!io_policy_valid(policy)||!document||!absolute_root||!output||*output ||
        !xtoml_owned_uses_policy(document,policy)) return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*policy,XR_MANIFEST_OK,diagnostic};
    if (!manifest_absolute(&ctx,absolute_root)) return ctx.status==XR_MANIFEST_OK?XR_MANIFEST_BAD_ARGUMENT:ctx.status;
    XrNativePackagePlan *plan=xr_native_package_plan_parse_impl(&ctx,document,absolute_root);
    if (ctx.status==XR_MANIFEST_OK&&!plan) return XR_MANIFEST_NOT_FOUND;
    if (ctx.status==XR_MANIFEST_OK&&plan&&!plan->valid) manifest_status(&ctx,XR_MANIFEST_INVALID);
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) { *output=plan;plan=NULL; }
    xr_native_package_plan_free_owned(plan);return ctx.status;
}
#define NATIVE_OWNED_FIND(name,type) \
XR_FUNC XrManifestStatus xr_native_package_find_##name##_owned(const XrNativePackagePlan *plan, \
    const char *name_value, const type **output) { \
    if (!plan||!name_value||!output)return XR_MANIFEST_BAD_ARGUMENT; \
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,NULL}; \
    const type *found=xr_native_package_find_##name##_impl(&ctx,plan,name_value); \
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1))*output=found; \
    return ctx.status; \
}
NATIVE_OWNED_FIND(unit,XrNativeUnit)
NATIVE_OWNED_FIND(symbol,XrNativeSymbol)
NATIVE_OWNED_FIND(export,XrCExportPlan)
NATIVE_OWNED_FIND(link_symbol,XrLinkSymbolPlan)
NATIVE_OWNED_FIND(entry,XrFreestandingEntryPlan)
#undef NATIVE_OWNED_FIND
static char *native_copy_optional(ManifestContext *ctx, const char *text) {
    return text?manifest_duplicate(ctx,text):NULL;
}
static void native_exports_free(XrCExportPlan *items, uint32_t count) {
    if (!items)return;
    for (uint32_t i=0;i<count;++i) {
        manifest_free(items[i].xray_name);manifest_free(items[i].symbol);
        manifest_free(items[i].visibility);manifest_free(items[i].abi);
    }
    manifest_free(items);
}
XR_FUNC XrManifestStatus xr_native_package_configure_c_exports_owned(XrNativePackagePlan *plan,
    const char *prefix, const char *excludes, XrManifestDiagnostic *diagnostic) {
    if (!plan)return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,diagnostic};
    XrNativePackagePlan staged=*plan;
    staged.exports=manifest_calloc(&ctx,plan->export_count,sizeof(*staged.exports));
    if (!staged.exports)return ctx.status;
    for (uint32_t i=0;i<plan->export_count;++i) {
        if (!manifest_work(&ctx,1))break;
        const XrCExportPlan *old=&plan->exports[i];XrCExportPlan *item=&staged.exports[i];
        item->header=old->header;item->xray_name=native_copy_optional(&ctx,old->xray_name);
        item->symbol=native_copy_optional(&ctx,old->symbol);
        item->visibility=native_copy_optional(&ctx,old->visibility);item->abi=native_copy_optional(&ctx,old->abi);
    }
    char error[256]={0};
    if (ctx.status==XR_MANIFEST_OK&&!xr_native_package_configure_c_exports_impl(&ctx,&staged,prefix,excludes,error,sizeof(error)))
        manifest_error(&ctx,error[0]?error:"invalid C export configuration");
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) {
        native_exports_free(plan->exports,plan->export_count);
        plan->exports=staged.exports;plan->export_count=staged.export_count;plan->fingerprint=staged.fingerprint;
        staged.exports=NULL;
    }
    native_exports_free(staged.exports,plan->export_count);return ctx.status;
}
static void native_layouts_free(XrNativeLayoutAssertion *items, uint32_t count) {
    if (!items)return;
    for (uint32_t i=0;i<count;++i) {
        native_free_string_array(items[i].field_names,items[i].field_count);
        manifest_free(items[i].field_offsets);
    }
    manifest_free(items);
}
static XrNativeLayoutAssertion *native_layouts_copy(ManifestContext *ctx, const XrNativePackagePlan *plan) {
    XrNativeLayoutAssertion *items=manifest_calloc(ctx,plan->layout_count,sizeof(*items));
    if (!items)return NULL;
    for (uint32_t i=0;i<plan->layout_count;++i) {
        if (!manifest_work(ctx,1))break;
        const XrNativeLayoutAssertion *old=&plan->layouts[i];XrNativeLayoutAssertion *item=&items[i];
        *item=*old;item->field_names=NULL;item->field_offsets=NULL;
        if (old->field_count) {
            item->field_names=manifest_calloc(ctx,old->field_count,sizeof(char *));
            item->field_offsets=manifest_calloc(ctx,old->field_count,sizeof(uint32_t));
            if (ctx->status!=XR_MANIFEST_OK)break;
            manifest_copy(ctx,item->field_offsets,old->field_offsets,(size_t)old->field_count*sizeof(uint32_t));
            for (uint32_t j=0;j<old->field_count;++j) {
                if (!manifest_work(ctx,1))break;
                item->field_names[j]=native_copy_optional(ctx,old->field_names[j]);
            }
        }
    }
    if (ctx->status!=XR_MANIFEST_OK) { native_layouts_free(items,plan->layout_count);return NULL; }
    return items;
}
XR_FUNC XrManifestStatus xr_native_package_note_layout_subject_owned(XrNativePackagePlan *plan, const char *name) {
    if (!plan||!name)return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,NULL};XrNativePackagePlan staged=*plan;
    staged.layouts=native_layouts_copy(&ctx,plan);
    if (!staged.layouts)return ctx.status;
    xr_native_package_note_layout_subject_impl(&ctx,&staged,name);
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) {
        native_layouts_free(plan->layouts,plan->layout_count);plan->layouts=staged.layouts;staged.layouts=NULL;
    }
    native_layouts_free(staged.layouts,staged.layout_count);return ctx.status;
}
XR_FUNC XrManifestStatus xr_native_package_resolve_layout_owned(XrNativePackagePlan *plan,
    const char *name, const XrAggregateLayout *layout, bool *output) {
    if (!plan||!name||!layout||!output||layout->field_count>XR_MAX_AGG_FIELDS)return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,NULL};XrNativePackagePlan staged=*plan;
    staged.layouts=native_layouts_copy(&ctx,plan);
    if (!staged.layouts)return ctx.status;
    bool found=xr_native_package_resolve_layout_impl(&ctx,&staged,name,layout);
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) {
        native_layouts_free(plan->layouts,plan->layout_count);plan->layouts=staged.layouts;staged.layouts=NULL;*output=found;
    }
    native_layouts_free(staged.layouts,staged.layout_count);return ctx.status;
}
XR_FUNC XrManifestStatus xr_native_package_validate_symbol_arity_owned(const XrNativePackagePlan *plan,
    const char *name, uint32_t arity, XrManifestDiagnostic *diagnostic) {
    if (!plan||!name)return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,diagnostic};char message[256]={0};
    if (!xr_native_package_validate_symbol_arity_impl(&ctx,plan,name,arity,message,sizeof(message)))
        manifest_error(&ctx,message[0]?message:"native symbol arity mismatch");
    return ctx.status;
}
XR_FUNC XrManifestStatus xr_native_package_explain_owned(const XrNativePackagePlan *plan, FILE *output) {
    if (!plan||!output)return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(plan),XR_MANIFEST_OK,NULL};
    xr_native_package_explain_impl(&ctx,plan,output);
    if (ferror(output))manifest_status(&ctx,XR_MANIFEST_IO);
    return ctx.status;
}
