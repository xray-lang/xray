/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain_update.c - Locked preference updates with retained cleanup
 */
#include "xcli_toolchain_update.h"
#include "../toolchain/xtc_xir_publication.h"
#include "../../base/xfileio.h"
#include "../../base/xtoml.h"
#include "../../os/os_fs.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif

#define UPDATE_BYTES (256 * 1024)
struct XrCliToolchainUpdate {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    XrCliToolchainUpdateRequest request;
    XrCliToolchainUpdateView view;
    char *path, *lock_path, *target, *compiler, *zig;
    uint8_t *bytes;
    size_t length, capacity;
    XrCliToolchainConfig *snapshot;
    XtcXirPublication *publication;
    XrFsExclusiveLock lock;
    bool executed;
};
static XrCliToolchainUpdateStatus update_fail(XrCliToolchainUpdate *owner, XrCliToolchainUpdateStatus status) {
    if (owner->view.first_error == XR_CLI_TOOLCHAIN_UPDATE_OK) owner->view.first_error = status;
    return owner->view.first_error;
}
static XrCliToolchainUpdateStatus update_io_status(XrOsIoStatus status) {
    return status == XR_OS_IO_OK ? XR_CLI_TOOLCHAIN_UPDATE_OK :
        status == XR_OS_IO_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY :
        status == XR_OS_IO_NOT_FOUND ? XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED :
        status == XR_OS_IO_UNSUPPORTED ? XR_CLI_TOOLCHAIN_UPDATE_UNSUPPORTED :
        status == XR_OS_IO_BAD_ARGUMENT ? XR_CLI_TOOLCHAIN_UPDATE_INVALID : XR_CLI_TOOLCHAIN_UPDATE_IO;
}
static bool update_io(XrCliToolchainUpdate *owner, XrOsIoStatus status) {
    return update_fail(owner, update_io_status(status)) == XR_CLI_TOOLCHAIN_UPDATE_OK;
}
static bool update_work(XrCliToolchainUpdate *owner, uint64_t units) {
    return owner->view.first_error == XR_CLI_TOOLCHAIN_UPDATE_OK &&
        update_io(owner, owner->policy.work(owner->policy.context, units));
}
static XrCliToolchainUpdateStatus update_config_status(XrCliToolchainConfigStatus status) {
    return status == XR_CLI_TOOLCHAIN_CONFIG_OK ? XR_CLI_TOOLCHAIN_UPDATE_OK :
        status == XR_CLI_TOOLCHAIN_CONFIG_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET :
        status == XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY :
        status == XR_CLI_TOOLCHAIN_CONFIG_IO ? XR_CLI_TOOLCHAIN_UPDATE_IO :
        status == XR_CLI_TOOLCHAIN_CONFIG_UNRESOLVED ? XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED : XR_CLI_TOOLCHAIN_UPDATE_INVALID;
}
static bool update_copy(XrCliToolchainUpdate *owner, const char *input, size_t limit, char **output) {
    if (!input) return true;
    size_t length = 0;
    while (update_work(owner, 1)) {
        if (!input[length]) break;
        if (++length > limit) { update_fail(owner, XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false; }
    }
    if (owner->view.first_error != XR_CLI_TOOLCHAIN_UPDATE_OK) return false;
    if (!length) { update_fail(owner, XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false; }
    if (!update_io(owner, owner->policy.alloc(owner->policy.context, length + 1, (void **)output)) ||
        !update_work(owner, length + 1)) return false;
    memcpy(*output, input, length + 1); return true;
}
static bool update_length(XrCliToolchainUpdate *owner, const char *text, size_t *output) {
    size_t length = 0;
    while (update_work(owner,1)) {
        if (!text[length]) { *output = length; return true; }
        if (++length > XR_PATH_LIMIT_MAX_PATH) { update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false; }
    }
    return false;
}
static bool update_equal(XrCliToolchainUpdate *owner, const char *left, const char *right, bool *output) {
    for (size_t i = 0; update_work(owner,2); ++i) {
        if (left[i] != right[i]) { *output = false; return true; }
        if (!left[i]) { *output = true; return true; }
    }
    return false;
}
static bool update_target(XrCliToolchainUpdate *owner) {
    if (!owner->target) return owner->request.reset;
    size_t length = 0;
    if (!update_length(owner,owner->target,&length) ||
        !update_work(owner,1 + length + sizeof(XrToolchainTarget))) return false;
    XrToolchainTarget target; char error[256];
    if (xtc_target_parse(owner->target,&target,error,sizeof(error))) return true;
    update_fail(owner, XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false;
}
static bool update_append(XrCliToolchainUpdate *owner, const char *text, size_t length) {
    if (length > UPDATE_BYTES - owner->length) { update_fail(owner, XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false; }
    size_t needed = owner->length + length;
    if (needed > owner->capacity) {
        size_t capacity = owner->capacity ? owner->capacity : 512;
        while (capacity < needed) { if (!update_work(owner,1)) return false; capacity *= 2; }
        XrCompileResourceStatus status = xr_compile_resources_resize(owner->resources, (void **)&owner->bytes, capacity);
        if (status != XR_COMPILE_RESOURCE_OK) {
            update_fail(owner, status == XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET :
                status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY : XR_CLI_TOOLCHAIN_UPDATE_INVALID);
            return false;
        }
        owner->capacity = capacity;
    }
    if (!update_work(owner, length)) return false;
    memcpy(owner->bytes + owner->length, text, length); owner->length = needed; return true;
}
#define UPDATE_TEXT(owner,text) update_append(owner,text,sizeof(text)-1)
static bool update_string(XrCliToolchainUpdate *owner, const char *text) {
    static const char hex[] = "0123456789abcdef";
    if (!UPDATE_TEXT(owner,"\"")) return false;
    for (size_t i = 0; update_work(owner, 1); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!c) return UPDATE_TEXT(owner,"\"");
        if (c == '"' || c == '\\') {
            char escape[2] = {'\\',(char)c}; if (!update_append(owner,escape,2)) return false;
        } else if (c < 0x20 || c == 0x7f) {
            char escape[6] = {'\\','u','0','0',hex[c>>4],hex[c&15]};
            if (!update_append(owner,escape,6)) return false;
        } else if (!update_append(owner,(const char *)&c,1)) return false;
    }
    return false;
}
static bool update_preference(XrCliToolchainUpdate *owner, const XrCliToolchainPreference *preference) {
    bool native = false;
    if (!update_equal(owner,preference->target,"native",&native)) return false;
    if (native) { if (!UPDATE_TEXT(owner,"\n[native]\nprovider = ")) return false; }
    else if (!UPDATE_TEXT(owner,"\n[target.") || !update_string(owner,preference->target) ||
        !UPDATE_TEXT(owner,"]\nprovider = ")) return false;
    if (!update_string(owner,xtc_selector_name(preference->selector)) || !UPDATE_TEXT(owner,"\n")) return false;
    if (preference->compiler && (!UPDATE_TEXT(owner,"compiler = ") ||
        !update_string(owner,preference->compiler) || !UPDATE_TEXT(owner,"\n"))) return false;
    return !preference->zig || (UPDATE_TEXT(owner,"zig = ") && update_string(owner,preference->zig) && UPDATE_TEXT(owner,"\n"));
}
static bool update_serialize(XrCliToolchainUpdate *owner) {
    const XrCliToolchainConfigView *view = xr_cli_toolchain_config_view(owner->snapshot);
    if (!UPDATE_TEXT(owner,"schema = 1\n")) return false;
    bool replaced = false; size_t count = 0, targets = 0;
    XrCliToolchainPreference replacement = {owner->target,owner->compiler,owner->zig,owner->request.selector};
    for (size_t i = 0; i < view->count; ++i) {
        const XrCliToolchainPreference *preference = &view->preferences[i];
        bool same = false, native = false;
        if (!update_equal(owner,preference->target,owner->target,&same)) return false;
        if (same) {
            replaced = true; if (owner->request.reset) continue; preference = &replacement;
        }
        if (!update_preference(owner,preference)) return false;
        if (!update_equal(owner,preference->target,"native",&native)) return false;
        ++count; if (!native) ++targets;
    }
    if (!replaced && !owner->request.reset) {
        if (!update_preference(owner,&replacement)) return false;
        bool native = false;
        if (!update_equal(owner,replacement.target,"native",&native)) return false;
        ++count; if (!native) ++targets;
    }
    if (count > 17 || targets > 16) { update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_INVALID); return false; }
    if (!UPDATE_TEXT(owner,"\n[policy]\nallow_fallback = ") ||
        !update_append(owner,view->allow_fallback ? "true" : "false",view->allow_fallback ? 4 : 5) ||
        !UPDATE_TEXT(owner,"\nprefer_external_native = ") ||
        !update_append(owner,view->prefer_external_native ? "true" : "false",view->prefer_external_native ? 4 : 5) ||
        !UPDATE_TEXT(owner,"\nauto_probe = ") ||
        !update_append(owner,view->auto_probe ? "true" : "false",view->auto_probe ? 4 : 5) || !UPDATE_TEXT(owner,"\n")) return false;
    XrTomlValue *parsed = NULL; XrTomlParseLimits limits = {UPDATE_BYTES,8};
    XrTomlParseStatus status = xtoml_parse_owned(&owner->policy,(const char *)owner->bytes,owner->length,&limits,&parsed);
    xtoml_owned_free(parsed);
    if (status != XR_TOML_PARSE_OK) {
        update_fail(owner,status == XR_TOML_PARSE_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET :
            status == XR_TOML_PARSE_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY : XR_CLI_TOOLCHAIN_UPDATE_INVALID);
        return false;
    }
    return true;
}
static bool update_directories(XrCliToolchainUpdate *owner) {
    char *parents[32] = {0}; size_t count = 0;
    bool okay = update_io(owner,xr_path_dirname_owned(&owner->policy,owner->path,&parents[0]));
    if (okay) count = 1;
    while (okay) {
        XrFsStat stat; XrOsIoStatus status = xr_os_io_stat(&owner->policy,parents[count-1],&stat);
        if (status == XR_OS_IO_OK) {
            if (stat.kind != XR_FS_DIR) okay = update_io(owner,XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        if (status != XR_OS_IO_NOT_FOUND) { okay = update_io(owner,status); break; }
        if (count == 32) { okay = false; update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_INVALID); break; }
        okay = update_io(owner,xr_path_dirname_owned(&owner->policy,parents[count-1],&parents[count]));
        bool same = false;
        if (okay) okay = update_equal(owner,parents[count-1],parents[count],&same);
        if (okay && same) {
            xr_compile_resources_free(parents[count]); parents[count] = NULL;
            okay = false; update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED); break;
        }
        if (okay) ++count;
        else { xr_compile_resources_free(parents[count]); parents[count] = NULL; }
    }
    for (size_t i = count; okay && i > 1; --i) okay = update_io(owner,xr_os_io_mkdir(&owner->policy,parents[i-2],0700));
    for (size_t i = 0; i < count; ++i) xr_compile_resources_free(parents[i]);
    return okay;
}
static bool update_publication(XrCliToolchainUpdate *owner, XtcXirPublicationStatus status) {
    return update_fail(owner,status == XTC_XIR_PUBLICATION_OK ? XR_CLI_TOOLCHAIN_UPDATE_OK :
        status == XTC_XIR_PUBLICATION_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET :
        status == XTC_XIR_PUBLICATION_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY :
        status == XTC_XIR_PUBLICATION_INVALID ? XR_CLI_TOOLCHAIN_UPDATE_INVALID :
        status == XTC_XIR_PUBLICATION_UNRESOLVED ? XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED :
        status == XTC_XIR_PUBLICATION_UNSUPPORTED ? XR_CLI_TOOLCHAIN_UPDATE_UNSUPPORTED : XR_CLI_TOOLCHAIN_UPDATE_IO) == XR_CLI_TOOLCHAIN_UPDATE_OK;
}
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_new(XrCompileResources *resources,
    const XrCliToolchainUpdateRequest *request, XrCliToolchainUpdate **output) {
    if (!resources || !request || !output || *output ||
        (!request->target && !request->reset) || request->selector > XR_TOOLCHAIN_SELECTOR_ZIG ||
        request->selector < XR_TOOLCHAIN_SELECTOR_AUTO ||
        (request->reset && (request->compiler || request->zig || request->selector != XR_TOOLCHAIN_SELECTOR_AUTO)) ||
        (request->compiler && request->selector == XR_TOOLCHAIN_SELECTOR_ZIG) ||
        (request->zig && request->selector != XR_TOOLCHAIN_SELECTOR_ZIG && request->selector != XR_TOOLCHAIN_SELECTOR_AUTO))
        return XR_CLI_TOOLCHAIN_UPDATE_INVALID;
#ifndef XR_OS_WINDOWS
    return XR_CLI_TOOLCHAIN_UPDATE_UNSUPPORTED;
#else
    XrCliToolchainUpdate *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) return allocated == XR_COMPILE_RESOURCE_BUDGET ?
        XR_CLI_TOOLCHAIN_UPDATE_BUDGET : allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ?
        XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY : XR_CLI_TOOLCHAIN_UPDATE_INVALID;
    owner->resources = resources; owner->policy = xr_compile_io_policy(resources);
    owner->request.selector = request->selector; owner->request.reset = request->reset;
    char *locator = NULL;
    bool okay = update_fail(owner,update_config_status(xr_cli_toolchain_config_path(resources,request->path,&locator))) == XR_CLI_TOOLCHAIN_UPDATE_OK;
    if (okay) okay = update_io(owner,xr_realpath_owned(&owner->policy,locator,&owner->path));
    xr_compile_resources_free(locator);
    if (okay) okay = update_copy(owner,request->target,127,&owner->target) && update_target(owner) &&
        update_copy(owner,request->compiler,XR_PATH_LIMIT_MAX_PATH,&owner->compiler) &&
        update_copy(owner,request->zig,XR_PATH_LIMIT_MAX_PATH,&owner->zig);
    if (okay) {
        XtcXirPublicationRequest prepare = {owner->path,{XR_PATH_LIMIT_MAX_PATH,8,UPDATE_BYTES}};
        okay = update_publication(owner,xtc_xir_publication_new(resources,&prepare,&owner->publication));
    }
    if (okay) {
        size_t length = 0;
        okay = update_length(owner,owner->path,&length) && update_work(owner,length + 6) &&
            update_io(owner,owner->policy.alloc(owner->policy.context,length + 6,(void **)&owner->lock_path));
        if (okay) { memcpy(owner->lock_path,owner->path,length); memcpy(owner->lock_path+length,".lock",6); }
    }
    if (okay) { owner->view.path = owner->path; *output = owner; return XR_CLI_TOOLCHAIN_UPDATE_OK; }
    XrCliToolchainUpdateStatus failure = owner->view.first_error;
    (void)xr_cli_toolchain_update_close(&owner); return failure;
#endif
}
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_execute(XrCliToolchainUpdate *owner) {
    if (!owner) return XR_CLI_TOOLCHAIN_UPDATE_INVALID;
    if (owner->view.first_error != XR_CLI_TOOLCHAIN_UPDATE_OK) return owner->view.first_error;
    if (owner->executed) return XR_CLI_TOOLCHAIN_UPDATE_INVALID;
    owner->executed = true;
    XrFsStat initial; XrOsIoStatus initial_status = xr_os_io_stat(&owner->policy,owner->path,&initial);
    if (initial_status != XR_OS_IO_NOT_FOUND && !update_io(owner,initial_status)) return owner->view.first_error;
    if (initial_status == XR_OS_IO_OK && initial.kind != XR_FS_FILE) return update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_INVALID);
    if (!update_directories(owner) || !update_io(owner,xr_os_io_lock_exclusive(&owner->policy,owner->lock_path,&owner->lock)))
        return owner->view.first_error;
    if (owner->request.reset && !owner->target) {
        XrFsStat stat; XrOsIoStatus status = xr_os_io_stat(&owner->policy,owner->path,&stat);
        if (status == XR_OS_IO_NOT_FOUND) return XR_CLI_TOOLCHAIN_UPDATE_OK;
        if (!update_io(owner,status) || stat.kind != XR_FS_FILE) return update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_INVALID);
        if (update_io(owner,xr_os_io_remove(&owner->policy,owner->path))) owner->view.removed = true;
        return owner->view.first_error;
    }
    if (update_fail(owner,update_config_status(xr_cli_toolchain_config_open(owner->resources,owner->path,&owner->snapshot)))
        != XR_CLI_TOOLCHAIN_UPDATE_OK) return owner->view.first_error;
    if (owner->request.reset && !xr_cli_toolchain_config_view(owner->snapshot)->exists) return XR_CLI_TOOLCHAIN_UPDATE_OK;
    if (!update_serialize(owner)) return owner->view.first_error;
    if (update_publication(owner,xtc_xir_publication_write(owner->publication,owner->bytes,owner->length)) &&
        update_publication(owner,xtc_xir_publication_commit(owner->publication))) owner->view.published = true;
    return owner->view.first_error;
}
XR_FUNC const XrCliToolchainUpdateView *xr_cli_toolchain_update_view(const XrCliToolchainUpdate *owner) {
    return owner ? &owner->view : NULL;
}
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_close(XrCliToolchainUpdate **handle) {
    if (!handle) return XR_CLI_TOOLCHAIN_UPDATE_INVALID;
    XrCliToolchainUpdate *owner = *handle;
    if (!owner) return XR_CLI_TOOLCHAIN_UPDATE_OK;
    XtcXirPublicationStatus closed = xtc_xir_publication_close(&owner->publication);
    if (closed != XTC_XIR_PUBLICATION_OK) {
        if (closed != XTC_XIR_PUBLICATION_PENDING) update_publication(owner,closed);
        return XR_CLI_TOOLCHAIN_UPDATE_PENDING;
    }
#ifdef XR_OS_WINDOWS
    if (owner->lock.handle) {
        /* This is the sole acquired handle. Close releases its range lock;
         * retaining it on OS failure keeps every remaining cleanup obligation. */
        if (!CloseHandle((HANDLE)owner->lock.handle)) {
            update_fail(owner,XR_CLI_TOOLCHAIN_UPDATE_IO); return XR_CLI_TOOLCHAIN_UPDATE_PENDING;
        }
        owner->lock.handle = 0;
    }
#endif
    xr_cli_toolchain_config_free(owner->snapshot);
    xr_compile_resources_free(owner->bytes); xr_compile_resources_free(owner->lock_path);
    xr_compile_resources_free(owner->target); xr_compile_resources_free(owner->compiler); xr_compile_resources_free(owner->zig);
    xr_compile_resources_free(owner->path); xr_compile_resources_free(owner); *handle = NULL;
    return XR_CLI_TOOLCHAIN_UPDATE_OK;
}
