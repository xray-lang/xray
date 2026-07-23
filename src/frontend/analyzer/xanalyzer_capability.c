/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 */

#include "xanalyzer_capability.h"

#include "xanalyzer_builtins.h"
#include "../../runtime/class/xclass_info.h"

#include <stdio.h>
#include <string.h>

static bool path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix)
        return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len || strcmp(path + path_len - suffix_len, suffix) != 0)
        return false;
    return path_len == suffix_len || path[path_len - suffix_len - 1] == '/' ||
           path[path_len - suffix_len - 1] == '\\';
}

typedef struct XaDeclaredTypeCapabilitySpec {
    const char *module_name;
    const char *declaration_name;
    uint32_t flags;
} XaDeclaredTypeCapabilitySpec;

static const XaDeclaredTypeCapabilitySpec declared_type_capabilities[] = {
#define XA_TYPE_CAPABILITY(module_name, declaration_name, flags)                                   \
    {module_name, declaration_name, flags},
#include "xa_type_capability_registry.def"
#undef XA_TYPE_CAPABILITY
};

static bool declaration_identity_matches(const char *actual, const char *declared) {
    if (!actual || !declared)
        return false;
    size_t declared_len = strlen(declared);
    return strncmp(actual, declared, declared_len) == 0 &&
           (actual[declared_len] == '\0' || actual[declared_len] == '$');
}

static bool path_is_canonical_stdlib_module(const char *path, const char *module_name) {
    if (!path || !module_name)
        return false;
    char suffix[160];
    int n = snprintf(suffix, sizeof(suffix), "stdlib/%s/%s.xr", module_name, module_name);
    if (n > 0 && (size_t) n < sizeof(suffix) && path_has_suffix(path, suffix))
        return true;
    n = snprintf(suffix, sizeof(suffix), "<embedded stdlib>/%s/%s.xr", module_name, module_name);
    return n > 0 && (size_t) n < sizeof(suffix) && path_has_suffix(path, suffix);
}

uint32_t xa_stdlib_type_capability_flags(const char *module_name, const char *declaration_name) {
    if (!module_name || !declaration_name)
        return XA_TYPE_CAP_NONE;
    for (size_t i = 0;
         i < sizeof(declared_type_capabilities) / sizeof(declared_type_capabilities[0]); i++) {
        const XaDeclaredTypeCapabilitySpec *spec = &declared_type_capabilities[i];
        if (strcmp(module_name, spec->module_name) == 0 &&
            declaration_identity_matches(declaration_name, spec->declaration_name))
            return spec->flags;
    }
    return XA_TYPE_CAP_NONE;
}

uint32_t xa_declared_type_capability_flags(const char *file_path, const char *declaration_name) {
    if (!file_path || !declaration_name)
        return XA_TYPE_CAP_NONE;
    for (size_t i = 0;
         i < sizeof(declared_type_capabilities) / sizeof(declared_type_capabilities[0]); i++) {
        const XaDeclaredTypeCapabilitySpec *spec = &declared_type_capabilities[i];
        if (declaration_identity_matches(declaration_name, spec->declaration_name) &&
            path_is_canonical_stdlib_module(file_path, spec->module_name))
            return spec->flags;
    }
    return XA_TYPE_CAP_NONE;
}

static uint32_t native_type_capability_flags(const XrType *type) {
    if (!type)
        return XA_TYPE_CAP_NONE;
    switch (xr_type_to_builtin_id((XrType *) type)) {
        case XR_TID_CHANNEL:
        case XR_TID_ATOMIC:
        case XR_TID_WORKQUEUE:
        case XR_TID_RESULTGROUP:
        case XR_TID_COUNTDOWNLATCH:
        case XR_TID_SEMAPHORE:
        case XR_TID_EVENTCOUNT:
            return XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;
        default:
            return XA_TYPE_CAP_NONE;
    }
}

uint32_t xa_type_capability_flags(const XrType *type) {
    uint32_t flags = native_type_capability_flags(type);
    if (type && XR_TYPE_IS_INSTANCE(type) && type->instance.class_ref)
        flags |= type->instance.class_ref->capability_flags;
    return flags;
}

bool xa_type_has_capabilities(const XrType *type, uint32_t required) {
    return (xa_type_capability_flags(type) & required) == required;
}
