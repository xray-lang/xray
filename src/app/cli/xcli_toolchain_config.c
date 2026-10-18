/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain_config.c - Strict preference snapshots on one caller ledger
 */
#include "xcli_toolchain_config.h"
#include "../../base/xfileio.h"
#include "../../base/xtoml.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#include <string.h>

#define CONFIG_INPUT_LIMIT (256 * 1024)
#define CONFIG_TARGET_LIMIT 17
struct XrCliToolchainConfig {
    XrCompileState *state;
    XrOsIoPolicy policy;
    XrTomlValue *root;
    char *path;
    XrCliToolchainConfigView view;
    XrCliToolchainPreference preferences[CONFIG_TARGET_LIMIT];
    XrCliToolchainConfigStatus status;
};
static XrCliToolchainConfigStatus config_resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_CLI_TOOLCHAIN_CONFIG_OK :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_TOOLCHAIN_CONFIG_BUDGET :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY : XR_CLI_TOOLCHAIN_CONFIG_INVALID;
}
static XrCliToolchainConfigStatus config_capture(XrCompileState *state,XrCliToolchainConfigStatus status) {
    if(status==XR_CLI_TOOLCHAIN_CONFIG_BUDGET || status==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY)
        return config_resource_status(xr_compile_state_fail(state,status==XR_CLI_TOOLCHAIN_CONFIG_BUDGET ?
            XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
    return status;
}
static XrCliToolchainConfigStatus config_io_status(XrOsIoStatus status) {
    return status == XR_OS_IO_OK ? XR_CLI_TOOLCHAIN_CONFIG_OK :
        status == XR_OS_IO_BUDGET ? XR_CLI_TOOLCHAIN_CONFIG_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY :
        status == XR_OS_IO_NOT_FOUND ? XR_CLI_TOOLCHAIN_CONFIG_UNRESOLVED :
        status == XR_OS_IO_BAD_ARGUMENT ? XR_CLI_TOOLCHAIN_CONFIG_INVALID : XR_CLI_TOOLCHAIN_CONFIG_IO;
}
static bool config_io(XrCliToolchainConfig *owner, XrOsIoStatus status) {
    if (owner->status == XR_CLI_TOOLCHAIN_CONFIG_OK) owner->status = config_io_status(status);
    return owner->status == XR_CLI_TOOLCHAIN_CONFIG_OK;
}
static bool config_work(XrCliToolchainConfig *owner, uint64_t units) {
    return owner->status == XR_CLI_TOOLCHAIN_CONFIG_OK &&
        config_io(owner, owner->policy.work(owner->policy.context, units));
}
static bool config_invalid(XrCliToolchainConfig *owner) {
    if (owner->status == XR_CLI_TOOLCHAIN_CONFIG_OK) owner->status = XR_CLI_TOOLCHAIN_CONFIG_INVALID;
    return false;
}
static bool config_parse_status(XrCliToolchainConfig *owner, XrTomlParseStatus status) {
    if (status == XR_TOML_PARSE_OK) return true;
    if (owner->status == XR_CLI_TOOLCHAIN_CONFIG_OK) owner->status =
        status == XR_TOML_PARSE_BUDGET ? XR_CLI_TOOLCHAIN_CONFIG_BUDGET :
        status == XR_TOML_PARSE_OUT_OF_MEMORY ? XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY :
        status == XR_TOML_PARSE_IO ? XR_CLI_TOOLCHAIN_CONFIG_IO : XR_CLI_TOOLCHAIN_CONFIG_INVALID;
    return false;
}
static bool config_get(XrCliToolchainConfig *owner, XrTomlValue *table,
    const char *key, XrTomlValue **output) {
    return config_parse_status(owner, xtoml_owned_get(table, key, output));
}
static bool config_known(XrCliToolchainConfig *owner, const XrTomlValue *table,
    const char *const *keys, size_t count) {
    if (!table || table->type != XR_TOML_TABLE) return config_invalid(owner);
    for (int i = 0; i < table->as.table.count; ++i) {
        const XrTomlMember *member = &table->as.table.members[i];
        bool known = false;
        for (size_t k = 0; k < count; ++k) {
            size_t n = strlen(keys[k]);
            if (!config_work(owner, n + 1)) return false;
            if (n == member->key_length && !memcmp(member->key, keys[k], n)) { known = true; break; }
        }
        if (!known) return config_invalid(owner);
    }
    return true;
}
static bool config_string(XrCliToolchainConfig *owner, const XrTomlValue *value,
    size_t limit, const char **output) {
    if (!value || value->type != XR_TOML_STRING || !value->string_length ||
        value->string_length > limit) return config_invalid(owner);
    if (!config_work(owner, value->string_length)) return false;
    if (memchr(value->as.string, 0, value->string_length)) return config_invalid(owner);
    *output = value->as.string; return true;
}
static bool config_preference(XrCliToolchainConfig *owner, XrTomlValue *table,
    const char *target) {
    static const char *const fields[] = {"provider", "compiler", "zig"};
    static const char *const selectors[] = {"auto", "host", "clang", "gcc", "msvc", "zig"};
    if (owner->view.count == CONFIG_TARGET_LIMIT ||
        !config_known(owner, table, fields, 3)) return config_invalid(owner);
    XrTomlValue *provider = NULL, *compiler = NULL, *zig = NULL;
    if (!config_get(owner, table, "provider", &provider) ||
        !config_get(owner, table, "compiler", &compiler) || !config_get(owner, table, "zig", &zig)) return false;
    const char *name = NULL;
    if (!config_string(owner, provider, 5, &name)) return false;
    XrCliToolchainPreference preference = {target, NULL, NULL, XR_TOOLCHAIN_SELECTOR_AUTO};
    bool found = false;
    for (size_t i = 0; i < 6; ++i) {
        size_t n = strlen(selectors[i]);
        if (!config_work(owner, n + 1)) return false;
        if (n == provider->string_length && !memcmp(name, selectors[i], n)) {
            preference.selector = (XrToolchainSelector)i; found = true; break;
        }
    }
    if (!found) return config_invalid(owner);
    if (compiler && !config_string(owner, compiler, XR_PATH_LIMIT_MAX_PATH, &preference.compiler)) return false;
    if (zig && !config_string(owner, zig, XR_PATH_LIMIT_MAX_PATH, &preference.zig)) return false;
    if ((preference.compiler && preference.selector == XR_TOOLCHAIN_SELECTOR_ZIG) ||
        (preference.zig && preference.selector != XR_TOOLCHAIN_SELECTOR_AUTO &&
            preference.selector != XR_TOOLCHAIN_SELECTOR_ZIG)) return config_invalid(owner);
    if (!config_work(owner, sizeof(preference))) return false;
    owner->preferences[owner->view.count++] = preference; return true;
}
static bool config_boolean(XrCliToolchainConfig *owner, XrTomlValue *table,
    const char *key, bool *output) {
    XrTomlValue *value = NULL;
    if (!config_get(owner, table, key, &value)) return false;
    if (!value) return true;
    if (value->type != XR_TOML_BOOL) return config_invalid(owner);
    *output = value->as.boolean; return true;
}
static bool config_target(XrCliToolchainConfig *owner, const XrTomlMember *member) {
    /* The model parser owns recognized triples. Its display list may omit a
     * valid host record and is not an admission directory. Charge this fixed
     * model lookup and its received text/result on the original ledger. */
    if (!config_work(owner, 1 + member->key_length + sizeof(XrToolchainTarget))) return false;
    XrToolchainTarget target; char error[256];
    return xtc_target_parse(member->key,&target,error,sizeof(error)) || config_invalid(owner);
}
static bool config_decode(XrCliToolchainConfig *owner) {
    static const char *const keys[] = {"schema", "native", "target", "policy"};
    if (!config_known(owner, owner->root, keys, 4)) return false;
    XrTomlValue *schema = NULL, *native = NULL, *targets = NULL, *policy = NULL;
    if (!config_get(owner, owner->root, "schema", &schema) ||
        !config_get(owner, owner->root, "native", &native) ||
        !config_get(owner, owner->root, "target", &targets) ||
        !config_get(owner, owner->root, "policy", &policy)) return false;
    if (!schema || schema->type != XR_TOML_INTEGER || schema->as.integer != 1) return config_invalid(owner);
    if (native && !config_preference(owner, native, "native")) return false;
    if (targets) {
        if (targets->type != XR_TOML_TABLE || targets->as.table.count > 16) return config_invalid(owner);
        for (int i = 0; i < targets->as.table.count; ++i) {
            XrTomlMember *member = &targets->as.table.members[i];
            if (!member->key_length || member->key_length > 127 ||
                !config_work(owner, member->key_length)) return config_invalid(owner);
            if (memchr(member->key, 0, member->key_length) || !strcmp(member->key, "native"))
                return config_invalid(owner);
            if (!config_target(owner, member)) return false;
            if (!config_preference(owner, member->value, member->key)) return false;
        }
    }
    if (policy) {
        static const char *const fields[] = {"allow_fallback", "prefer_external_native", "auto_probe"};
        if (!config_known(owner, policy, fields, 3) ||
            !config_boolean(owner, policy, fields[0], &owner->view.allow_fallback) ||
            !config_boolean(owner, policy, fields[1], &owner->view.prefer_external_native) ||
            !config_boolean(owner, policy, fields[2], &owner->view.auto_probe)) return false;
    }
    return true;
}
static bool config_path(XrCliToolchainConfig *owner, const char *path) {
    if (path) {
        size_t length = 0;
        while (config_work(owner, 1)) {
            if (!path[length]) break;
            if (++length > XR_PATH_LIMIT_MAX_PATH) return config_invalid(owner);
        }
        if (owner->status != XR_CLI_TOOLCHAIN_CONFIG_OK) return false;
        if (!length) return config_invalid(owner);
        if (!config_io(owner, owner->policy.alloc(owner->policy.context, length + 1,
            (void **)&owner->path)) || !config_work(owner, length + 1)) return false;
        memcpy(owner->path, path, length + 1); return true;
    }
    char *base = NULL;
#ifdef XR_OS_WINDOWS
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "LOCALAPPDATA", &base);
    const char *suffix = "Xray/toolchains.toml";
#else
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "XDG_CONFIG_HOME", &base);
    const char *suffix = "xray/toolchains.toml";
    if (status == XR_OS_IO_NOT_FOUND || (status == XR_OS_IO_OK && !base[0])) {
        xr_compile_resources_free(base); base = NULL;
        status = xr_os_io_environment_get(&owner->policy, "HOME", &base);
        suffix = ".config/xray/toolchains.toml";
    }
#endif
    bool okay = config_io(owner, status);
    if (okay && !base[0]) { owner->status = XR_CLI_TOOLCHAIN_CONFIG_UNRESOLVED; okay = false; }
    if (okay) okay = config_io(owner, xr_path_join_owned(&owner->policy, base, suffix, &owner->path));
    xr_compile_resources_free(base); return okay;
}
XR_FUNC XrCliToolchainConfigStatus xr_cli_toolchain_config_open(
    XrCompileState *state, const char *path, XrCliToolchainConfig **output) {
    if (!state || !output || *output) return XR_CLI_TOOLCHAIN_CONFIG_INVALID;
    XrCliToolchainConfig *owner = NULL;
    XrCompileResourceStatus status = xr_compile_state_calloc(state, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_COMPILE_RESOURCE_OK) return config_resource_status(status);
    status=xr_compile_state_retain(state);
    if(status!=XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(owner);return config_resource_status(xr_compile_state_fail(state,status));
    }
    owner->state=state;
    XrCompileResources *resources=xr_compile_state_resources(state);
    owner->policy = xr_compile_io_policy(resources);
    owner->view.preferences = owner->preferences;
    owner->view.allow_fallback = owner->view.prefer_external_native = owner->view.auto_probe = true;
    uint8_t *bytes = NULL; size_t length = 0;
    bool okay = config_path(owner, path);
    if (okay) {
        XrOsIoStatus read = xr_os_io_read_regular_file(&owner->policy, owner->path, CONFIG_INPUT_LIMIT, &bytes, &length);
        if (read != XR_OS_IO_NOT_FOUND) {
            okay = config_io(owner, read);
            if (okay) {
                owner->view.exists = true;
                XrTomlParseLimits limits = {CONFIG_INPUT_LIMIT, 8};
                okay = config_parse_status(owner, xtoml_parse_owned(&owner->policy,
                    (const char *)bytes, length, &limits, &owner->root)) && config_decode(owner);
            }
        }
    }
    xr_compile_resources_free(bytes);
    if (okay && config_work(owner, sizeof(owner->view))) {
        owner->view.path = owner->path; *output = owner; return XR_CLI_TOOLCHAIN_CONFIG_OK;
    }
    XrCliToolchainConfigStatus failure = config_capture(state,owner->status);
    xr_cli_toolchain_config_free(owner); return failure;
}
XR_FUNC const XrCliToolchainConfigView *xr_cli_toolchain_config_view(const XrCliToolchainConfig *owner) {
    return owner ? &owner->view : NULL;
}
XR_FUNC XrCliToolchainConfigStatus xr_cli_toolchain_config_path(
    XrCompileState *state, const char *path, char **output) {
    if (!state || !output || *output) return XR_CLI_TOOLCHAIN_CONFIG_INVALID;
    XrCompileResourceStatus status=xr_compile_state_status(state);
    if(status!=XR_COMPILE_RESOURCE_OK)return config_resource_status(status);
    XrCompileResources *resources=xr_compile_state_resources(state);
    XrCliToolchainConfig temporary = {0};
    temporary.policy = xr_compile_io_policy(resources);
    if (config_path(&temporary, path)) { *output = temporary.path; return XR_CLI_TOOLCHAIN_CONFIG_OK; }
    xr_compile_resources_free(temporary.path); return config_capture(state,temporary.status);
}
XR_FUNC void xr_cli_toolchain_config_free(XrCliToolchainConfig *owner) {
    if (!owner) return;
    XrCompileState *state=owner->state;
    xtoml_owned_free(owner->root); xr_compile_resources_free(owner->path);
    xr_compile_resources_free(owner);xr_compile_state_release(state);
}
