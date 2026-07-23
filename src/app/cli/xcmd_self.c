/*
 * xray - Lightweight typed scripting with native concurrency
 * xcmd_self.c - Provider-aware update and uninstall delegation
 */

#include "xcli_spec.h"
#include "xcli_diag.h"
#include "xcli_installation.h"
#include "../toolchain/xtc_discovery.h"
#include "../toolchain/xtc_process.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#include "../../base/xchecks.h"

#include <stdio.h>
#include <string.h>

typedef enum XrSelfProvider {
    XR_SELF_PROVIDER_MANAGED = 0,
    XR_SELF_PROVIDER_DEB,
    XR_SELF_PROVIDER_RPM,
    XR_SELF_PROVIDER_MACOS_PKG,
    XR_SELF_PROVIDER_HOMEBREW,
    XR_SELF_PROVIDER_PORTABLE,
} XrSelfProvider;

static bool self_parent(char *path) {
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    char *separator = slash;
    if (backslash && (!separator || backslash > separator))
        separator = backslash;
    if (!separator || separator == path)
        return false;
    *separator = '\0';
    return true;
}

static bool self_join(char *out, size_t out_size, const char *root, const char *relative) {
    int written = snprintf(out, out_size, "%s/%s", root, relative);
    return written >= 0 && (size_t) written < out_size;
}

static bool self_find_managed(const char *executable, char *manager, size_t manager_size) {
    char ancestor[XR_PATH_MAX];
    snprintf(ancestor, sizeof(ancestor), "%s", executable);
    if (!self_parent(ancestor))
        return false;
    for (int depth = 0; depth < 7; depth++) {
        char state[XR_PATH_MAX];
        char candidate[XR_PATH_MAX];
        if (self_join(state, sizeof(state), ancestor, "state/install-state.json") &&
            self_join(candidate, sizeof(candidate), ancestor,
#ifdef XR_OS_WINDOWS
                      "bin/xrayup.exe"
#else
                      "bin/xrayup"
#endif
                      ) &&
            xr_fs_is_file(state) && xr_fs_is_file(candidate)) {
            snprintf(manager, manager_size, "%s", candidate);
            return true;
        }
        if (!self_parent(ancestor))
            break;
    }
    return false;
}

static bool self_query(const char *program, const char *const *args, size_t count, char *output,
                       size_t output_size) {
    char executable[XR_PATH_MAX];
    char err[256];
    XrProcessSpec spec;
    XrProcessResult result;
    if (!xtc_find_executable(program, executable, sizeof(executable)))
        return false;
    xtc_process_spec_init(&spec, executable, 5000);
    for (size_t i = 0; i < count && i + 2 < XTC_PROCESS_MAX_ARGS; i++)
        spec.argv[i + 1] = args[i];
    spec.argv[count + 1] = NULL;
    spec.output_limit = 16384;
    if (!xtc_process_run(&spec, &result, err, sizeof(err)))
        return false;
    bool ok = !result.timed_out && result.exit_code == 0;
    if (ok && output && output_size)
        snprintf(output, output_size, "%s", result.stdout_data ? result.stdout_data : "");
    xtc_process_result_free(&result);
    return ok;
}

static void self_trim_line(char *text) {
    if (!text)
        return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ' ||
                       text[len - 1] == '\t'))
        text[--len] = '\0';
}

static XrSelfProvider self_detect_provider(const char *executable, char *manager,
                                           size_t manager_size) {
    char output[512];
    const char *dpkg_args[] = {"-S", executable};
    const char *rpm_args[] = {"-qf", "--qf", "%{NAME}", executable};
    const char *pkg_args[] = {"--file-info", executable};
    const char *brew_args[] = {"--prefix", "xray-lang"};
    if (self_find_managed(executable, manager, manager_size))
        return XR_SELF_PROVIDER_MANAGED;
    if (self_query("dpkg-query", dpkg_args, 2, output, sizeof(output)) &&
        strncmp(output, "xray-lang:", 10) == 0)
        return XR_SELF_PROVIDER_DEB;
    if (self_query("rpm", rpm_args, 4, output, sizeof(output))) {
        self_trim_line(output);
        if (strcmp(output, "xray-lang") == 0)
            return XR_SELF_PROVIDER_RPM;
    }
    if (self_query("pkgutil", pkg_args, 2, output, sizeof(output)) &&
        strstr(output, "pkgid: org.xray-lang."))
        return XR_SELF_PROVIDER_MACOS_PKG;
    if (self_query("brew", brew_args, 2, output, sizeof(output))) {
        self_trim_line(output);
        size_t prefix_len = strlen(output);
        if (prefix_len > 0 && strncmp(executable, output, prefix_len) == 0 &&
            (executable[prefix_len] == '/' || executable[prefix_len] == '\\'))
            return XR_SELF_PROVIDER_HOMEBREW;
    }
    return XR_SELF_PROVIDER_PORTABLE;
}

static int self_run_manager(const char *manager, const char *action, bool json_output) {
    const char *argv[] = {manager, action, json_output ? "--json" : NULL, NULL};
    XrProcId pid = xr_proc_spawn(manager, argv);
    int code = -1;
    if (pid == XR_PROC_INVALID || xr_proc_wait(pid, &code) != 0) {
        xr_cli_error("self", "failed to start managed installer '%s'", manager);
        return XR_CLI_EXIT_FAIL;
    }
    return code;
}

static const char *self_provider_name(XrSelfProvider provider) {
    switch (provider) {
        case XR_SELF_PROVIDER_MANAGED:
            return "managed-user";
        case XR_SELF_PROVIDER_DEB:
            return "deb";
        case XR_SELF_PROVIDER_RPM:
            return "rpm";
        case XR_SELF_PROVIDER_MACOS_PKG:
            return "macos-pkg";
        case XR_SELF_PROVIDER_HOMEBREW:
            return "homebrew";
        case XR_SELF_PROVIDER_PORTABLE:
            return "portable-or-source";
    }
    return "unknown";
}

static const char *self_delegation(XrSelfProvider provider, const char *action) {
    bool update = strcmp(action, "update") == 0;
    switch (provider) {
        case XR_SELF_PROVIDER_DEB:
            return update ? "sudo apt update && sudo apt install --only-upgrade xray-lang "
                            "xray-lang-sdk"
                          : "sudo apt remove xray-lang-dev xray-lang-sdk xray-lang";
        case XR_SELF_PROVIDER_RPM:
            return update ? "sudo dnf upgrade xray-lang xray-lang-sdk"
                          : "sudo dnf remove xray-lang-full xray-lang-dev xray-lang-sdk xray-lang";
        case XR_SELF_PROVIDER_MACOS_PKG:
            return update ? "download and run the signed Xray macOS PKG"
                          : "/Library/Developer/Xray/bin/xray-uninstall";
        case XR_SELF_PROVIDER_HOMEBREW:
            return update ? "brew upgrade xray-lang" : "brew uninstall xray-lang";
        case XR_SELF_PROVIDER_PORTABLE:
            return update ? "replace this portable/source installation using its original provider"
                          : "remove this portable/source installation using its original manifest";
        case XR_SELF_PROVIDER_MANAGED:
            break;
    }
    return "";
}

XR_FUNC int cmd_self(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count != 1 || (strcmp(inv->positionals[0], "update") != 0 &&
                                       strcmp(inv->positionals[0], "uninstall") != 0)) {
        xr_cli_error("self", "expected 'update' or 'uninstall'");
        return XR_CLI_EXIT_USAGE;
    }
    char executable[XR_PATH_MAX];
    char manager[XR_PATH_MAX] = {0};
    if (xr_proc_self_exe_path(executable, sizeof(executable)) != 0) {
        xr_cli_error("self", "cannot resolve the active Xray executable");
        return XR_CLI_EXIT_FAIL;
    }
    XrSelfProvider provider = self_detect_provider(executable, manager, sizeof(manager));
    bool json_output = inv->ctx->json_output || xr_cli_opt_bool(&inv->options, "json");
    if (provider == XR_SELF_PROVIDER_MANAGED)
        return self_run_manager(manager, inv->positionals[0], json_output);

    const char *command = self_delegation(provider, inv->positionals[0]);
    if (json_output) {
        printf("{\"schema\":1,\"provider\":\"%s\",\"action\":\"%s\",\"delegationRequired\":true,"
               "\"command\":\"%s\"}\n",
               self_provider_name(provider), inv->positionals[0], command);
    } else {
        printf("Active provider: %s\nDelegation required: %s\n", self_provider_name(provider),
               command);
    }
    return XR_CLI_EXIT_DELEGATE;
}

XR_FUNC int cmd_doctor(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count != 1 || strcmp(inv->positionals[0], "installation") != 0) {
        xr_cli_error("doctor", "expected subcommand 'installation'");
        return XR_CLI_EXIT_USAGE;
    }
    return xr_cli_print_installation_info(inv->ctx->json_output ||
                                          xr_cli_opt_bool(&inv->options, "json"));
}
