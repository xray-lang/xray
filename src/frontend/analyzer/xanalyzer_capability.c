/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 */

#include "xanalyzer_capability.h"

#include "xanalyzer_builtins.h"
#include "../../runtime/class/xclass_info.h"

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

static bool name_in_set(const char *name, const char *const *set, size_t count) {
    if (!name)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, set[i]) == 0)
            return true;
    }
    return false;
}

uint32_t xa_declared_type_capability_flags(const char *file_path, const char *declaration_name) {
    static const char *const sync_roots[] = {"Mutex", "RwLock", "Once", "Barrier", "Condvar"};
    static const char *const sys_roots[] = {"ThreadLocal"};
    const uint32_t sync_capability = XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;

    if ((path_has_suffix(file_path, "stdlib/sync/sync.xr") ||
         path_has_suffix(file_path, "<embedded stdlib>/sync/sync.xr")) &&
        name_in_set(declaration_name, sync_roots, sizeof(sync_roots) / sizeof(sync_roots[0])))
        return sync_capability;

    if ((path_has_suffix(file_path, "stdlib/sys/sys.xr") ||
         path_has_suffix(file_path, "<embedded stdlib>/sys/sys.xr")) &&
        name_in_set(declaration_name, sys_roots, sizeof(sys_roots) / sizeof(sys_roots[0])))
        return sync_capability;

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
