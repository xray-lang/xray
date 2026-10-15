/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain_config.h - Owned toolchain preference snapshots
 */
#ifndef XCLI_TOOLCHAIN_CONFIG_H
#define XCLI_TOOLCHAIN_CONFIG_H
#include "../../base/xcompile_resources.h"
#include "../toolchain/xtc_model.h"

typedef struct XrCliToolchainConfig XrCliToolchainConfig;
typedef struct XrCliToolchainPreference {
    const char *target, *compiler, *zig;
    XrToolchainSelector selector;
} XrCliToolchainPreference;
typedef struct XrCliToolchainConfigView {
    const char *path;
    bool exists, allow_fallback, prefer_external_native, auto_probe;
    const XrCliToolchainPreference *preferences;
    size_t count;
} XrCliToolchainConfigView;
typedef enum XrCliToolchainConfigStatus {
    XR_CLI_TOOLCHAIN_CONFIG_OK, XR_CLI_TOOLCHAIN_CONFIG_INVALID,
    XR_CLI_TOOLCHAIN_CONFIG_BUDGET, XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY,
    XR_CLI_TOOLCHAIN_CONFIG_IO, XR_CLI_TOOLCHAIN_CONFIG_UNRESOLVED
} XrCliToolchainConfigStatus;

/* Read one bounded regular-file snapshot on the required original ledger.
 * NULL path selects the actual platform preference path. Missing files are an
 * empty configuration; malformed files never become an automatic selection.
 * Failure preserves output. All request borrows end on return. A snapshot
 * outlives its input bytes and the caller's ledger reference. Preferences are
 * configuration facts, not provider readiness or execution authority. */
XR_FUNC XrCliToolchainConfigStatus xr_cli_toolchain_config_open(
    XrCompileResources *resources, const char *path, XrCliToolchainConfig **output);
/* Resolve the same preference locator without reading its contents. This lets
 * an explicit reset repair a corrupt configuration. Output must be NULL; the
 * returned string owns its original ledger and uses resources_free. */
XR_FUNC XrCliToolchainConfigStatus xr_cli_toolchain_config_path(
    XrCompileResources *resources, const char *path, char **output);
/* Immutable borrows remain valid until free and do not consume work. */
XR_FUNC const XrCliToolchainConfigView *xr_cli_toolchain_config_view(const XrCliToolchainConfig *owner);
XR_FUNC void xr_cli_toolchain_config_free(XrCliToolchainConfig *owner);
#endif // XCLI_TOOLCHAIN_CONFIG_H
