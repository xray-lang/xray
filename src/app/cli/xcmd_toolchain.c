/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain.c - 'xray toolchain' provider/configuration diagnostics
 */

#include "xcli_spec.h"
#include "xcli_diag.h"
#include "../../base/xchecks.h"
#include "../toolchain/xtc_config.h"
#include "../toolchain/xtc_discovery.h"
#include "../toolchain/xtc_json.h"
#include "../toolchain/xtc_probe.h"
#include "../toolchain/xtc_probe_cache.h"

#include "xray_version.h"

#include <stdio.h>
#include <string.h>

#ifndef XRAY_BUILD_COMMIT
#define XRAY_BUILD_COMMIT "unknown"
#endif

static bool xcmd_toolchain_json(const XrCliInvocation *inv) {
    return (inv->ctx && inv->ctx->json_output) || xr_cli_opt_bool(&inv->options, "json");
}

#define xcmd_json_string xtc_json_write_string

static bool xcmd_toolchain_target(const XrCliInvocation *inv, const char **requested,
                                  XrToolchainTarget *target) {
    char err[256];
    *requested = xr_cli_opt_string(&inv->options, "target", "native");
    if (!xtc_target_parse(*requested, target, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return false;
    }
    return true;
}

static bool xcmd_toolchain_load_config(char *path, size_t path_size, XrToolchainConfig *config,
                                       char *err, size_t err_size) {
    bool exists = false;
    if (!xtc_config_path(path, path_size, err, err_size))
        return false;
    return xtc_config_load(path, config, &exists, err, err_size);
}

static bool xcmd_toolchain_request(const XrCliInvocation *inv, const char **requested,
                                   XrToolchainProbeOptions *options, char *err, size_t err_size) {
    memset(options, 0, sizeof(*options));
    *requested = xr_cli_opt_string(&inv->options, "target", "native");
    if (!xtc_target_parse(*requested, &options->request.target, err, err_size))
        return false;

    const char *provider = xr_cli_opt_string(&inv->options, "provider", NULL);
    const char *cc = xr_cli_opt_string(&inv->options, "cc", NULL);
    const char *zig = xr_cli_opt_string(&inv->options, "zig", NULL);
    char config_path[1200];
    XrToolchainConfig config;
    if (!xcmd_toolchain_load_config(config_path, sizeof(config_path), &config, err, err_size))
        return false;
    const XrToolchainPreference *preference = xtc_config_find(&config, *requested);
    if (!provider) {
        if (preference) {
            options->request.selector = preference->selector;
        } else {
            options->request.selector = XR_TOOLCHAIN_SELECTOR_AUTO;
        }
    } else if (!xtc_selector_parse(provider, &options->request.selector, err, err_size)) {
        return false;
    }

    const char *resolved_cc = cc;
    const char *resolved_zig = zig;
    xtc_config_apply_provider_paths(preference, options->request.selector, cc, getenv("CC"), zig,
                                    getenv("XRAY_ZIG"), &resolved_cc, &resolved_zig);
    if (resolved_cc != cc) {
        snprintf(options->cc_storage, sizeof(options->cc_storage), "%s", resolved_cc);
        cc = options->cc_storage;
    }
    if (resolved_zig != zig) {
        snprintf(options->zig_storage, sizeof(options->zig_storage), "%s", resolved_zig);
        zig = options->zig_storage;
    }

    if (cc && options->request.selector == XR_TOOLCHAIN_SELECTOR_ZIG) {
        snprintf(err, err_size, "--cc cannot be used with provider 'zig'");
        return false;
    }
    if (zig && options->request.selector != XR_TOOLCHAIN_SELECTOR_AUTO &&
        options->request.selector != XR_TOOLCHAIN_SELECTOR_ZIG) {
        snprintf(err, err_size, "--zig cannot be used with provider '%s'",
                 xtc_selector_name(options->request.selector));
        return false;
    }
    options->request.cc = cc ? cc : getenv("CC");
    options->request.zig = zig;
    options->request.program_hint = inv->ctx ? inv->ctx->program : NULL;
    const char *profile = xr_cli_opt_string(
        &inv->options, "profile",
        xtc_target_is_hosted(&options->request.target) ? "hosted" : "freestanding");
    if (!xtc_profile_parse(profile, &options->profile, err, err_size))
        return false;
    options->no_run = xr_cli_opt_bool(&inv->options, "no-run");
    options->refresh = xr_cli_opt_bool(&inv->options, "refresh");
    options->keep_probe = xr_cli_opt_bool(&inv->options, "keep-probe");
    options->required_codegen_capabilities = XR_TOOLCHAIN_CODEGEN_ALL;
    return true;
}

static void xcmd_toolchain_print_probe_human(const XrToolchainProbeOptions *options,
                                             const XrToolchainProbeResult *result, bool ready) {
    printf("AOT toolchain: %s\n", ready ? "READY" : "UNAVAILABLE");
    printf("  Target:     %s\n", options->request.target.name);
    printf("  Provider:   %s (%s)\n", xtc_provider_name(result->selection.provider),
           xtc_ownership_name(result->selection.ownership));
    printf("  Compiler:   %s\n", result->selection.program ? result->selection.program : "missing");
    if (result->selection.version[0])
        printf("  Version:    %s\n", result->selection.version);
    printf("  Readiness:  %s\n", xtc_readiness_name(result->selection.readiness));
    printf("  C compile:  %s\n", xtc_capability_state_name(result->c_compile));
    printf("  SDK compile:%s\n", xtc_capability_state_name(result->sdk_compile));
    printf("  Runtime link: %s\n", xtc_capability_state_name(result->runtime_link));
    printf("  Native run: %s\n", xtc_capability_state_name(result->native_run));
    printf("  LTO:        %s\n", xtc_capability_state_name(result->lto));
    printf("  Force inline: %s\n", xtc_capability_state_name(result->force_inline));
    printf("  Preserve call: %s\n", xtc_capability_state_name(result->preserve_call));
    printf("  Value opaque: %s\n", xtc_capability_state_name(result->value_opaque));
    printf("  Compiler fence: %s\n", xtc_capability_state_name(result->compiler_fence));
    for (size_t i = 0; i < result->diagnostic_count; i++)
        printf("  [%s] %s: %s\n", xtc_reason_code_name(result->diagnostics[i].code),
               result->diagnostics[i].stage, result->diagnostics[i].message);
}

static int xcmd_toolchain_probe_or_doctor(const XrCliInvocation *inv) {
    XrToolchainProbeOptions options;
    XrToolchainProbeResult result;
    const char *requested = NULL;
    char err[512] = {0};
    if (!xcmd_toolchain_request(inv, &requested, &options, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_USAGE;
    }
    memset(&result, 0, sizeof(result));
    bool ready = xtc_probe(&options, &result, err, sizeof(err));
    if (xcmd_toolchain_json(inv))
        (void) xtc_probe_json_write(stdout, requested, &options, &result, ready,
                                    XRAY_VERSION_STRING, XRAY_BUILD_COMMIT);
    else
        xcmd_toolchain_print_probe_human(&options, &result, ready);
    return ready ? XR_CLI_EXIT_OK : XR_CLI_EXIT_UNAVAILABLE;
}

static int xcmd_toolchain_list(const XrCliInvocation *inv) {
    const char *requested;
    XrToolchainTarget target;
    if (!xcmd_toolchain_target(inv, &requested, &target))
        return XR_CLI_EXIT_USAGE;
    char config_path[1200];
    char err[512];
    XrToolchainConfig config;
    if (!xcmd_toolchain_load_config(config_path, sizeof(config_path), &config, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_FAIL;
    }
    const XrToolchainPreference *preference = xtc_config_find(&config, requested);
    XrToolchainProbeCacheEntry cache_entries[XTC_PROBE_CACHE_MAX_ENTRIES];
    size_t cache_count = 0;
    if (!xtc_probe_cache_list(target.name, cache_entries, XTC_PROBE_CACHE_MAX_ENTRIES, &cache_count,
                              err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_FAIL;
    }
    static const XrToolchainProviderId providers[] = {
        XR_TOOLCHAIN_PROVIDER_APPLE_CLANG, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
        XR_TOOLCHAIN_PROVIDER_GCC, XR_TOOLCHAIN_PROVIDER_MSVC, XR_TOOLCHAIN_PROVIDER_ZIG};
    if (xcmd_toolchain_json(inv)) {
        printf("{\"schema\":1,\"target\":");
        xcmd_json_string(stdout, requested);
        printf(",\"normalizedTarget\":");
        xcmd_json_string(stdout, target.name);
        printf(",\"configPath\":");
        xcmd_json_string(stdout, config_path);
        printf(",\"preference\":");
        if (preference)
            xcmd_json_string(stdout, xtc_selector_name(preference->selector));
        else
            fputs("null", stdout);
        printf(",\"providers\":[");
        for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
            if (i)
                putchar(',');
            printf("{\"id\":");
            xcmd_json_string(stdout, xtc_provider_name(providers[i]));
            printf(",\"ownership\":\"external-or-managed\"}");
        }
        printf("],\"cache\":[");
        for (size_t i = 0; i < cache_count; i++) {
            if (i)
                putchar(',');
            printf("{\"key\":");
            xcmd_json_string(stdout, cache_entries[i].key);
            printf(",\"provider\":");
            xcmd_json_string(stdout, cache_entries[i].provider);
            printf(",\"fingerprint\":");
            xcmd_json_string(stdout, cache_entries[i].fingerprint);
            printf(",\"runtimeArtifact\":");
            xcmd_json_string(stdout, cache_entries[i].runtime_artifact);
            printf(",\"ready\":%s}", cache_entries[i].ready ? "true" : "false");
        }
        printf("]}\n");
    } else {
        printf("AOT providers for %s (%s):\n", requested, target.name);
        for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++)
            printf("  %s\n", xtc_provider_name(providers[i]));
        printf("Preference: %s\n", preference ? xtc_selector_name(preference->selector) : "auto");
        printf("Config: %s\n", config_path);
        if (cache_count == 0) {
            printf("Probe cache: none\n");
        } else {
            printf("Probe cache:\n");
            for (size_t i = 0; i < cache_count; i++)
                printf("  %s provider=%s ready=%s\n", cache_entries[i].key,
                       cache_entries[i].provider, cache_entries[i].ready ? "yes" : "no");
        }
    }
    return XR_CLI_EXIT_OK;
}

static int xcmd_toolchain_detect(const XrCliInvocation *inv) {
    XrToolchainProbeOptions options;
    const char *requested = NULL;
    char err[512];
    if (!xcmd_toolchain_request(inv, &requested, &options, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_USAGE;
    }
    XrToolchainCandidates candidates;
    if (!xtc_discover_candidates(&options.request, &candidates, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_UNAVAILABLE;
    }
    for (size_t i = 0; i < candidates.count; i++) {
        char version_err[256];
        (void) xtc_candidate_read_version(&candidates.items[i], version_err, sizeof(version_err));
    }
    if (xcmd_toolchain_json(inv)) {
        printf("{\"schema\":1,\"target\":");
        xcmd_json_string(stdout, requested);
        printf(",\"normalizedTarget\":");
        xcmd_json_string(stdout, options.request.target.name);
        printf(",\"candidates\":[");
        for (size_t i = 0; i < candidates.count; i++) {
            if (i)
                putchar(',');
            printf("{\"provider\":");
            xcmd_json_string(stdout, xtc_provider_name(candidates.items[i].provider));
            printf(",\"ownership\":");
            xcmd_json_string(stdout, xtc_ownership_name(candidates.items[i].ownership));
            printf(",\"compiler\":");
            xcmd_json_string(stdout, candidates.items[i].executable);
            printf(",\"version\":");
            xcmd_json_string(stdout, candidates.items[i].version);
            printf(",\"runnable\":%s}", candidates.items[i].runnable ? "true" : "false");
        }
        printf("]}\n");
    } else {
        printf("Detected candidates for %s:\n", options.request.target.name);
        for (size_t i = 0; i < candidates.count; i++)
            printf("  %-12s %-8s %s%s%s\n", xtc_provider_name(candidates.items[i].provider),
                   candidates.items[i].runnable ? "runnable" : "missing",
                   candidates.items[i].executable, candidates.items[i].version[0] ? " - " : "",
                   candidates.items[i].version);
    }
    return XR_CLI_EXIT_OK;
}

static int xcmd_toolchain_use(const XrCliInvocation *inv) {
    if (inv->positional_count != 2) {
        xr_cli_error("toolchain", "usage: xray toolchain use --target TARGET PROVIDER");
        return XR_CLI_EXIT_USAGE;
    }
    const char *target_text = xr_cli_opt_string(&inv->options, "target", NULL);
    if (!target_text) {
        xr_cli_error("toolchain", "--target is required for 'use'");
        return XR_CLI_EXIT_USAGE;
    }
    XrToolchainSelector selector;
    char err[512];
    XrToolchainTarget target;
    if (!xtc_target_parse(target_text, &target, err, sizeof(err)) ||
        !xtc_selector_parse(inv->positionals[1], &selector, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_USAGE;
    }
    const char *cc = xr_cli_opt_string(&inv->options, "cc", NULL);
    const char *zig = xr_cli_opt_string(&inv->options, "zig", NULL);
    if ((cc && selector == XR_TOOLCHAIN_SELECTOR_ZIG) ||
        (zig && selector != XR_TOOLCHAIN_SELECTOR_ZIG)) {
        xr_cli_error("toolchain", "provider/path option mismatch");
        return XR_CLI_EXIT_USAGE;
    }
    char path[1200];
    if (!xtc_config_path(path, sizeof(path), err, sizeof(err)) ||
        !xtc_config_use(path, target_text, selector, cc, zig, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_FAIL;
    }
    if (xcmd_toolchain_json(inv)) {
        printf("{\"schema\":1,\"updated\":true,\"target\":");
        xcmd_json_string(stdout, target_text);
        printf(",\"provider\":");
        xcmd_json_string(stdout, xtc_selector_name(selector));
        printf(",\"configPath\":");
        xcmd_json_string(stdout, path);
        printf("}\n");
    } else {
        printf("Toolchain preference updated: %s -> %s\n", target_text,
               xtc_selector_name(selector));
    }
    return XR_CLI_EXIT_OK;
}

static int xcmd_toolchain_reset(const XrCliInvocation *inv) {
    if (inv->positional_count != 1) {
        xr_cli_error("toolchain", "usage: xray toolchain reset [--target TARGET]");
        return XR_CLI_EXIT_USAGE;
    }
    const char *target = xr_cli_opt_string(&inv->options, "target", NULL);
    XrToolchainTarget parsed;
    char err[512];
    if (target && !xtc_target_parse(target, &parsed, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_USAGE;
    }
    char path[1200];
    const char *normalized_target = target ? parsed.name : NULL;
    if (!xtc_config_path(path, sizeof(path), err, sizeof(err)) ||
        !xtc_config_reset(path, target, err, sizeof(err)) ||
        !xtc_probe_cache_reset(normalized_target, err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_FAIL;
    }
    if (xcmd_toolchain_json(inv)) {
        printf("{\"schema\":1,\"reset\":true,\"target\":");
        if (target)
            xcmd_json_string(stdout, target);
        else
            fputs("null", stdout);
        printf("}\n");
    } else {
        printf("Toolchain preference reset: %s\n", target ? target : "all");
    }
    return XR_CLI_EXIT_OK;
}

static int xcmd_toolchain_config_path(const XrCliInvocation *inv) {
    if (inv->positional_count != 1) {
        xr_cli_error("toolchain", "usage: xray toolchain config-path");
        return XR_CLI_EXIT_USAGE;
    }
    char path[1200];
    char err[256];
    if (!xtc_config_path(path, sizeof(path), err, sizeof(err))) {
        xr_cli_error("toolchain", "%s", err);
        return XR_CLI_EXIT_FAIL;
    }
    if (xcmd_toolchain_json(inv)) {
        printf("{\"schema\":1,\"path\":");
        xcmd_json_string(stdout, path);
        printf("}\n");
    } else {
        printf("%s\n", path);
    }
    return XR_CLI_EXIT_OK;
}

XR_FUNC int cmd_toolchain(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count < 1) {
        xr_cli_error("toolchain", "missing subcommand");
        return XR_CLI_EXIT_USAGE;
    }
    const char *subcommand = inv->positionals[0];
    if (strcmp(subcommand, "list") == 0)
        return inv->positional_count == 1 ? xcmd_toolchain_list(inv) : XR_CLI_EXIT_USAGE;
    if (strcmp(subcommand, "detect") == 0)
        return inv->positional_count == 1 ? xcmd_toolchain_detect(inv) : XR_CLI_EXIT_USAGE;
    if (strcmp(subcommand, "probe") == 0 || strcmp(subcommand, "doctor") == 0)
        return inv->positional_count == 1 ? xcmd_toolchain_probe_or_doctor(inv) : XR_CLI_EXIT_USAGE;
    if (strcmp(subcommand, "use") == 0)
        return xcmd_toolchain_use(inv);
    if (strcmp(subcommand, "reset") == 0)
        return xcmd_toolchain_reset(inv);
    if (strcmp(subcommand, "config-path") == 0)
        return xcmd_toolchain_config_path(inv);
    xr_cli_error("toolchain", "unknown subcommand '%s'", subcommand);
    return XR_CLI_EXIT_USAGE;
}
