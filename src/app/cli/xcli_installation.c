/*
 * xcli_installation.c - Installed payload discovery and reporting.
 *
 * A payload is installed only when the executable lives under <root>/bin and
 * the versioned install marker exists.  PATH position and environment
 * variables are deliberately not treated as ownership evidence.
 */

#include "xcli_installation.h"
#include "xcli_help.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#include <stdio.h>
#include <string.h>

typedef struct XrInstallationInfo {
    bool executable_known;
    bool installed;
    bool payload_manifest_present;
    char executable[XR_PATH_MAX];
    char root[XR_PATH_MAX];
    char marker[XR_PATH_MAX];
    char payload_manifest[XR_PATH_MAX];
} XrInstallationInfo;

static bool path_dirname(char *path) {
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
        path[--len] = '\0';
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    char *separator = slash;
    if (backslash && (!separator || backslash > separator))
        separator = backslash;
    if (!separator)
        return false;
    if (separator == path) {
        path[1] = '\0';
    } else {
        *separator = '\0';
    }
    return true;
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *separator = slash;
    if (backslash && (!separator || backslash > separator))
        separator = backslash;
    return separator ? separator + 1 : path;
}

static bool join_path(char *out, size_t out_size, const char *root, const char *relative) {
    int written = snprintf(out, out_size, "%s/%s", root, relative);
    return written >= 0 && (size_t) written < out_size;
}

static void detect_installation(XrInstallationInfo *info) {
    memset(info, 0, sizeof(*info));
    if (xr_proc_self_exe_path(info->executable, sizeof(info->executable)) != 0)
        return;
    info->executable_known = true;

    char bin_dir[XR_PATH_MAX];
    snprintf(bin_dir, sizeof(bin_dir), "%s", info->executable);
    if (!path_dirname(bin_dir) || strcmp(path_basename(bin_dir), "bin") != 0)
        return;

    snprintf(info->root, sizeof(info->root), "%s", bin_dir);
    if (!path_dirname(info->root)) {
        info->root[0] = '\0';
        return;
    }
    if (!join_path(info->marker, sizeof(info->marker), info->root,
                   "share/xray/install/install-marker.json") ||
        !join_path(info->payload_manifest, sizeof(info->payload_manifest), info->root,
                   "share/xray/install/payload-manifest.json")) {
        info->root[0] = '\0';
        return;
    }
    info->installed = xr_fs_is_file(info->marker);
    info->payload_manifest_present = xr_fs_is_file(info->payload_manifest);
}

static void print_json_string(FILE *out, const char *value) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) value; *p; p++) {
        switch (*p) {
            case '"':
                fputs("\\\"", out);
                break;
            case '\\':
                fputs("\\\\", out);
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
                if (*p < 0x20)
                    fprintf(out, "\\u%04x", (unsigned) *p);
                else
                    fputc((int) *p, out);
        }
    }
    fputc('"', out);
}

int xr_cli_print_installation_info(bool json) {
    XrInstallationInfo info;
    detect_installation(&info);

    if (json) {
        fputs("{\"schema\":1,\"build\":", stdout);
        xr_cli_print_build_identity_json(stdout, 0);
        fputs(",\"installation\":{\"kind\":", stdout);
        print_json_string(stdout, info.installed ? "payload" : "source-or-standalone");
        fputs(",\"installed\":", stdout);
        fputs(info.installed ? "true" : "false", stdout);
        fputs(",\"executable\":", stdout);
        if (info.executable_known)
            print_json_string(stdout, info.executable);
        else
            fputs("null", stdout);
        fputs(",\"root\":", stdout);
        if (info.root[0])
            print_json_string(stdout, info.root);
        else
            fputs("null", stdout);
        fputs(",\"marker\":", stdout);
        if (info.installed)
            print_json_string(stdout, info.marker);
        else
            fputs("null", stdout);
        fputs(",\"payloadManifest\":", stdout);
        if (info.payload_manifest_present)
            print_json_string(stdout, info.payload_manifest);
        else
            fputs("null", stdout);
        fputs("}}\n", stdout);
        return 0;
    }

    puts("Installation:");
    printf("  Kind: %s\n", info.installed ? "installed payload" : "source/standalone build");
    if (info.executable_known)
        printf("  Executable: %s\n", info.executable);
    if (info.root[0])
        printf("  Root: %s\n", info.root);
    if (info.installed)
        printf("  Marker: %s\n", info.marker);
    printf("  Payload manifest: %s\n",
           info.payload_manifest_present ? info.payload_manifest : "not found");
    return 0;
}
