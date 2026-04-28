/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpkg_client.c - Package registry client implementation
 *
 * KEY CONCEPT:
 *   HTTP client for pkg.xray-lang.org registry. Handles package search,
 *   download, install, and publish operations with authentication support.
 *
 * NOTE: This file requires network support (XR_HAS_NETWORK)
 */

#include "xpkg_client.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../base/xjson.h"
#include "../base/xchecks.h"

#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)

#include "../../stdlib/http/http_client.h"
#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
#include "../../stdlib/crypto/crypto.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef XR_OS_WINDOWS
#include <sys/stat.h>  // chmod
#endif
#include "../os/os_fs.h"
#include "../os/os_proc.h"

// Thread-local config for multi-Isolate support
static XR_THREAD_LOCAL XrPkgClientConfig tls_config = {
    .registry_url = PKG_REGISTRY_URL, .auth_token = NULL, .timeout_ms = 30000, .verbose = false};

/*
 * Create directory recursively (like mkdir -p).
 * Safe implementation without shell command injection.
 */
static bool mkdir_recursive(const char *path) {
    if (!path || !*path)
        return false;

    char *tmp = xr_strdup(path);
    if (!tmp)
        return false;

    size_t len = strlen(tmp);

    // Remove trailing slash
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    // Create each directory in path
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (xr_fs_mkdir(tmp, 0755) != 0) {
                xr_free(tmp);
                return false;
            }
            *p = '/';
        }
    }

    // Create final directory
    if (xr_fs_mkdir(tmp, 0755) != 0) {
        xr_free(tmp);
        return false;
    }

    xr_free(tmp);
    return true;
}

/*
 * Execute a child process. Inherits parent stdio. Returns the exit
 * code on success, -1 on spawn / wait failure or abnormal exit.
 * Routed through xr_proc_* so the call site is portable.
 */
static int exec_command(const char *prog, char *const argv[]) {
    XrProcId pid = xr_proc_spawn(prog, (const char *const *) argv);
    if (pid == XR_PROC_INVALID) {
        return -1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0) {
        return -1;
    }
    return code;
}

/*
 * Extract tarball safely without shell injection.
 */
static bool extract_tarball(const char *tarball, const char *dest_dir, bool verbose) {
    // Build argument list for tar command
    char *argv[6];
    int argc = 0;

    argv[argc++] = "tar";
    argv[argc++] = "-xzf";
    argv[argc++] = (char *) tarball;
    argv[argc++] = "-C";
    argv[argc++] = (char *) dest_dir;
    argv[argc] = NULL;

    if (verbose) {
        printf("Extract: tar -xzf %s -C %s\n", tarball, dest_dir);
    }

    int ret = exec_command("tar", argv);
    return ret == 0;
}

/*
 * Extract a string value from JSON body by key.
 * Returns xr_strdup'd string (caller frees), or NULL if not found.
 */
static char *json_get_string(const char *json, const char *key) {
    if (!json || !key)
        return NULL;
    XrJsonValue *root = xjson_parse(json, strlen(json));
    if (!root)
        return NULL;
    const char *val = xjson_get_string(root, key);
    char *result = val ? xr_strdup(val) : NULL;
    xjson_free(root);
    return result;
}

/*
 * Extract a string array from JSON body by key.
 * Returns malloc'd array of xr_strdup'd strings (caller frees each + array).
 */
static char **json_get_string_array(const char *json, const char *key, int *count) {
    *count = 0;
    if (!json || !key)
        return NULL;
    XrJsonValue *root = xjson_parse(json, strlen(json));
    if (!root)
        return NULL;
    XrJsonValue *arr = xjson_get_array(root, key);
    if (!arr) {
        xjson_free(root);
        return NULL;
    }
    int n = xjson_array_len(arr);
    if (n == 0) {
        xjson_free(root);
        return NULL;
    }
    char **result = (char **) xr_malloc((size_t) n * sizeof(char *));
    if (!result) {
        xjson_free(root);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        XrJsonValue *elem = xjson_array_get(arr, i);
        if (elem && xjson_is_string(elem)) {
            result[*count] = xr_strdup(elem->as.string);
            (*count)++;
        }
    }
    xjson_free(root);
    return result;
}

/* ========== Client API Implementation ========== */

// Thread-local Isolate pointer for multi-Isolate support
static XR_THREAD_LOCAL XrayIsolate *tls_isolate = NULL;

bool xr_pkg_client_init(void) {
    // Connection pool is now per-isolate, no global init needed
    return true;
}

void xr_pkg_client_cleanup(void) {
    // Connection pool is now per-isolate, cleaned up with isolate
    tls_isolate = NULL;
}

void xr_pkg_client_set_config(const XrPkgClientConfig *config) {
    if (config) {
        tls_config = *config;
    }
}

void xr_pkg_client_set_isolate(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "pkg_client_set_isolate: NULL isolate");
    tls_isolate = isolate;
}

/* ========== HTTP API Implementation ========== */

XrPkgResponse *xr_pkg_http_get(const char *url) {
    XR_DCHECK(url != NULL, "pkg_http_get: NULL url");
    XrPkgResponse *resp = (XrPkgResponse *) xr_calloc(1, sizeof(XrPkgResponse));
    if (!resp)
        return NULL;

    if (tls_config.verbose) {
        printf("GET: %s\n", url);
    }

    // Use xray built-in HTTP client
    XrHttpResult result = xr_http_get(tls_isolate, url);

    resp->status_code = result.status_code;
    resp->success =
        (result.error == XR_HTTP_OK && result.status_code >= 200 && result.status_code < 300);

    if (result.body && result.body_len > 0) {
        resp->body = xr_strdup(result.body);
        resp->body_len = result.body_len;
    }

    if (!resp->success) {
        if (result.error_msg) {
            resp->error = xr_strdup(result.error_msg);
        } else if (result.error != XR_HTTP_OK) {
            resp->error = xr_strdup(xr_http_error_string(result.error));
        } else if (resp->body) {
            // Try to extract error message from response
            char *msg = json_get_string(resp->body, "error");
            if (msg) {
                resp->error = msg;
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "HTTP %d", resp->status_code);
                resp->error = xr_strdup(buf);
            }
        }
    }

    xr_http_result_free(&result);
    return resp;
}

XrPkgResponse *xr_pkg_http_post(const char *url, const char *body, const char *content_type) {
    XrPkgResponse *resp = (XrPkgResponse *) xr_calloc(1, sizeof(XrPkgResponse));
    if (!resp)
        return NULL;

    if (tls_config.verbose) {
        printf("POST: %s\n", url);
    }

    const char *ct = content_type ? content_type : "application/json";
    size_t body_len = body ? strlen(body) : 0;

    // Use xray built-in HTTP client
    XrHttpResult result = xr_http_post(tls_isolate, url, body, body_len, ct);

    resp->status_code = result.status_code;
    resp->success =
        (result.error == XR_HTTP_OK && result.status_code >= 200 && result.status_code < 300);

    if (result.body && result.body_len > 0) {
        resp->body = xr_strdup(result.body);
        resp->body_len = result.body_len;
    }

    if (!resp->success) {
        if (result.error_msg) {
            resp->error = xr_strdup(result.error_msg);
        } else if (result.error != XR_HTTP_OK) {
            resp->error = xr_strdup(xr_http_error_string(result.error));
        }
    }

    xr_http_result_free(&result);
    return resp;
}

bool xr_pkg_http_download_file(const char *url, const char *dest_path) {
    XR_DCHECK(url != NULL, "pkg_http_download_file: NULL url");
    XR_DCHECK(dest_path != NULL, "pkg_http_download_file: NULL dest_path");
    if (tls_config.verbose) {
        printf("Download: %s -> %s\n", url, dest_path);
    }

    // Use xray built-in HTTP client
    XrHttpResult result = xr_http_get(tls_isolate, url);

    if (result.error != XR_HTTP_OK || result.status_code < 200 || result.status_code >= 300) {
        if (tls_config.verbose) {
            fprintf(stderr, "Download failed: %s\n",
                    result.error_msg ? result.error_msg : xr_http_error_string(result.error));
        }
        xr_http_result_free(&result);
        return false;
    }

    // Write to file
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        xr_http_result_free(&result);
        return false;
    }

    if (result.body && result.body_len > 0) {
        fwrite(result.body, 1, result.body_len, f);
    }
    fclose(f);

    xr_http_result_free(&result);
    return true;
}

void xr_pkg_response_free(XrPkgResponse *response) {
    if (!response)
        return;
    xr_free(response->body);
    xr_free(response->error);
    xr_free(response);
}

/* ========== Package Info API Implementation ========== */

XrPackageInfo *xr_pkg_client_get_info(const char *owner, const char *name) {
    if (!owner || !name)
        return NULL;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/packages/%s/%s", tls_config.registry_url, owner, name);

    XrPkgResponse *resp = xr_pkg_http_get(url);
    if (!resp || !resp->success) {
        if (resp) {
            if (tls_config.verbose && resp->error) {
                fprintf(stderr, "Failed to get package info: %s\n", resp->error);
            }
            xr_pkg_response_free(resp);
        }
        return NULL;
    }

    // Parse JSON response
    XrPackageInfo *info = (XrPackageInfo *) xr_calloc(1, sizeof(XrPackageInfo));
    if (!info) {
        xr_pkg_response_free(resp);
        return NULL;
    }

    // Construct full package name
    char full_name[256];
    snprintf(full_name, sizeof(full_name), "%s/%s", owner, name);
    info->name = xr_strdup(full_name);

    // Parse response body once
    XrJsonValue *root = xjson_parse(resp->body, resp->body_len);
    if (root) {
        // Extract version list
        XrJsonValue *ver_arr = xjson_get_array(root, "versions");
        if (ver_arr) {
            int n = xjson_array_len(ver_arr);
            if (n > 0) {
                info->versions = (char **) xr_malloc((size_t) n * sizeof(char *));
                if (info->versions) {
                    for (int i = 0; i < n; i++) {
                        XrJsonValue *elem = xjson_array_get(ver_arr, i);
                        if (elem && xjson_is_string(elem)) {
                            info->versions[info->version_count++] = xr_strdup(elem->as.string);
                        }
                    }
                }
            }
        }

        // Extract dependency list
        XrJsonValue *dep_arr = xjson_get_array(root, "dependencies");
        if (dep_arr) {
            int n = xjson_array_len(dep_arr);
            if (n > 0) {
                info->deps = (char **) xr_malloc((size_t) n * sizeof(char *));
                if (info->deps) {
                    for (int i = 0; i < n; i++) {
                        XrJsonValue *elem = xjson_array_get(dep_arr, i);
                        if (elem && xjson_is_string(elem)) {
                            info->deps[info->dep_count++] = xr_strdup(elem->as.string);
                        }
                    }
                }
            }
        }

        // Extract latest_version
        const char *latest = xjson_get_string(root, "latest_version");
        if (latest)
            info->latest_version = xr_strdup(latest);

        // Extract description
        const char *desc = xjson_get_string(root, "description");
        if (desc)
            info->description = xr_strdup(desc);

        xjson_free(root);
    }

    xr_pkg_response_free(resp);
    return info;
}

bool xr_pkg_client_get_versions(const char *owner, const char *name, char ***versions, int *count) {
    if (!owner || !name || !versions || !count)
        return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/packages/%s/%s/versions", tls_config.registry_url, owner,
             name);

    XrPkgResponse *resp = xr_pkg_http_get(url);
    if (!resp || !resp->success) {
        if (resp)
            xr_pkg_response_free(resp);
        return false;
    }

    *versions = json_get_string_array(resp->body, "versions", count);

    xr_pkg_response_free(resp);
    return *versions != NULL;
}

XrPkgSearchResult *xr_pkg_client_search(const char *query) {
    if (!query)
        return NULL;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/search?q=%s", tls_config.registry_url, query);

    XrPkgResponse *resp = xr_pkg_http_get(url);
    if (!resp || !resp->success) {
        if (resp)
            xr_pkg_response_free(resp);
        return NULL;
    }

    XrPkgSearchResult *result = (XrPkgSearchResult *) xr_calloc(1, sizeof(XrPkgSearchResult));
    if (!result) {
        xr_pkg_response_free(resp);
        return NULL;
    }

    result->names = json_get_string_array(resp->body, "packages", &result->count);
    result->descriptions = json_get_string_array(resp->body, "descriptions", &result->count);

    xr_pkg_response_free(resp);
    return result;
}

void xr_pkg_search_result_free(XrPkgSearchResult *result) {
    if (!result)
        return;

    for (int i = 0; i < result->count; i++) {
        xr_free(result->names[i]);
        if (result->descriptions)
            xr_free(result->descriptions[i]);
    }
    xr_free(result->names);
    xr_free(result->descriptions);
    xr_free(result);
}

/* ========== Download API Implementation ========== */

bool xr_pkg_client_download(const char *owner, const char *name, const char *version,
                            const char *dest_dir) {
    if (!owner || !name || !version || !dest_dir)
        return false;

    // Ensure destination directory exists
    xr_fs_mkdir(dest_dir, 0755);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/packages/%s/%s/%s/download", tls_config.registry_url, owner,
             name, version);

    char dest_path[512];
    snprintf(dest_path, sizeof(dest_path), "%s/%s-%s.tar.gz", dest_dir, name, version);

    return xr_pkg_http_download_file(url, dest_path);
}

bool xr_pkg_client_install(const char *owner, const char *name, const char *version,
                           const char *dest_dir) {
    if (!owner || !name || !version || !dest_dir)
        return false;

    // Validate inputs to prevent path traversal
    if (strchr(owner, '/') || strchr(owner, '\\') || strchr(name, '/') || strchr(name, '\\') ||
        strchr(version, '/') || strchr(version, '\\')) {
        fprintf(stderr, "Invalid package identifier\n");
        return false;
    }

    // 1. Download package
    const char *home = getenv("HOME");
    if (!home)
        return false;

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.xray/cache", home);
    if (!mkdir_recursive(cache_dir)) {
        fprintf(stderr, "Failed to create cache directory: %s\n", cache_dir);
        return false;
    }

    char tarball[512];
    snprintf(tarball, sizeof(tarball), "%s/%s-%s-%s.tar.gz", cache_dir, owner, name, version);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/packages/%s/%s/%s/download", tls_config.registry_url, owner,
             name, version);

    if (!xr_pkg_http_download_file(url, tarball)) {
        fprintf(stderr, "Download failed: %s\n", url);
        return false;
    }

    // 2. Create destination directory (safe, no shell injection)
    char pkg_dir[512];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s/%s/%s", dest_dir, owner, name, version);

    if (!mkdir_recursive(pkg_dir)) {
        fprintf(stderr, "Failed to create package directory: %s\n", pkg_dir);
        return false;
    }

    // 3. Extract (safe, using fork/exec instead of system())
    if (!extract_tarball(tarball, pkg_dir, tls_config.verbose)) {
        fprintf(stderr, "Extract failed: %s\n", tarball);
        return false;
    }

    printf("Installed %s/%s@%s\n", owner, name, version);
    return true;
}

/* ========== Publish API Implementation ========== */

// Append a multipart text field to a dynamic buffer.
// Returns new write offset, or 0 on failure.
static size_t multipart_append_field(char **buf, size_t *cap, size_t off, const char *boundary,
                                     const char *name, const char *value) {
    if (!value)
        return off;
    char hdr[256];
    int hdr_len =
        snprintf(hdr, sizeof(hdr), "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n",
                 boundary, name);
    size_t val_len = strlen(value);
    size_t need = off + (size_t) hdr_len + val_len + 2;  // +2 for \r\n
    if (need > *cap) {
        size_t new_cap = need * 2;
        char *tmp = (char *) xr_realloc(*buf, new_cap);
        if (!tmp)
            return 0;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + off, hdr, hdr_len);
    off += hdr_len;
    memcpy(*buf + off, value, val_len);
    off += val_len;
    memcpy(*buf + off, "\r\n", 2);
    off += 2;
    return off;
}

bool xr_pkg_client_publish(const char *tarball_path, const char *auth_token,
                           const XrPkgPublishInfo *info) {
    if (!tarball_path || !auth_token || !info) {
        fprintf(stderr, "Error: tarball, auth token and publish info required\n");
        return false;
    }
    XR_DCHECK(info->name != NULL, "publish: NULL name");
    XR_DCHECK(info->version != NULL, "publish: NULL version");

    // Read tarball content
    size_t file_size;
    char *file_content = xr_file_read_all(tarball_path, "rb", &file_size);
    if (!file_content) {
        fprintf(stderr, "Error: cannot read package file: %s\n", tarball_path);
        return false;
    }

    const char *boundary = "----XrayPkgUploadBoundary";

    // Build multipart body: text fields + file part + footer
    size_t cap = file_size + 4096;
    char *body = (char *) xr_malloc(cap);
    if (!body) {
        xr_free(file_content);
        return false;
    }
    size_t off = 0;

    // Metadata fields (match server PostForm keys)
    off = multipart_append_field(&body, &cap, off, boundary, "name", info->name);
    off = multipart_append_field(&body, &cap, off, boundary, "version", info->version);
    off = multipart_append_field(&body, &cap, off, boundary, "description", info->description);
    off = multipart_append_field(&body, &cap, off, boundary, "license", info->license);
    if (off == 0) {
        xr_free(body);
        xr_free(file_content);
        return false;
    }

    // File part
    const char *filename = strrchr(tarball_path, '/');
    filename = filename ? filename + 1 : tarball_path;

    char file_hdr[512];
    int file_hdr_len =
        snprintf(file_hdr, sizeof(file_hdr),
                 "--%s\r\n"
                 "Content-Disposition: form-data; name=\"tarball\"; filename=\"%s\"\r\n"
                 "Content-Type: application/gzip\r\n\r\n",
                 boundary, filename);

    char footer[64];
    int footer_len = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t need = off + (size_t) file_hdr_len + file_size + (size_t) footer_len;
    if (need > cap) {
        char *tmp = (char *) xr_realloc(body, need);
        if (!tmp) {
            xr_free(body);
            xr_free(file_content);
            return false;
        }
        body = tmp;
        cap = need;
    }
    memcpy(body + off, file_hdr, file_hdr_len);
    off += file_hdr_len;
    memcpy(body + off, file_content, file_size);
    off += file_size;
    memcpy(body + off, footer, footer_len);
    off += footer_len;
    xr_free(file_content);

    size_t body_size = off;

    // Build URL and headers
    char url[512];
    snprintf(url, sizeof(url), "%s/api/packages", tls_config.registry_url);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    char auth_header_value[512];
    snprintf(auth_header_value, sizeof(auth_header_value), "Bearer %s", auth_token);

    XrHttpHeader headers[2] = {{.name = "Content-Type", .value = content_type},
                               {.name = "Authorization", .value = auth_header_value}};

    if (tls_config.verbose) {
        printf("Publish: POST %s (%zu bytes)\n", url, body_size);
    }

    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_POST;
    config.body = body;
    config.body_len = body_size;
    config.headers = headers;
    config.header_count = 2;
    config.timeout_ms = tls_config.timeout_ms;

    XrHttpResult result = xr_http_request(tls_isolate, &config);
    xr_free(body);

    bool success =
        (result.error == XR_HTTP_OK && result.status_code >= 200 && result.status_code < 300);

    if (success) {
        printf("Published successfully\n");
    } else {
        fprintf(stderr, "Publish failed");
        if (result.body && result.body_len > 0) {
            // Try to extract error message from JSON response
            char *msg = json_get_string(result.body, "error");
            if (msg) {
                fprintf(stderr, ": %s", msg);
                xr_free(msg);
            } else if (result.status_code > 0) {
                fprintf(stderr, " (HTTP %d)", result.status_code);
            }
        } else if (result.error_msg) {
            fprintf(stderr, ": %s", result.error_msg);
        } else if (result.status_code > 0) {
            fprintf(stderr, " (HTTP %d)", result.status_code);
        }
        fprintf(stderr, "\n");
    }

    xr_http_result_free(&result);
    return success;
}

/* ========== Auth API Implementation ========== */

bool xr_pkg_client_login(char **token_out) {
    if (!token_out)
        return false;

    printf("Please visit the following link to login:\n");
    printf("  %s/auth/cli\n\n", tls_config.registry_url);
    printf("After login, enter your token: ");
    fflush(stdout);

    char token[256] = {0};
    if (!fgets(token, sizeof(token), stdin)) {
        return false;
    }

    // Remove newline
    size_t len = strlen(token);
    if (len > 0 && token[len - 1] == '\n') {
        token[len - 1] = '\0';
    }

    if (token[0] == '\0') {
        fprintf(stderr, "Error: token cannot be empty\n");
        return false;
    }

    *token_out = xr_strdup(token);
    return true;
}

bool xr_pkg_client_save_token(const char *token) {
    if (!token)
        return false;

    const char *home = getenv("HOME");
    if (!home)
        return false;

    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%s/.xray", home);
    xr_fs_mkdir(config_dir, 0755);

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/.xray/credentials", home);

    FILE *f = fopen(config_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write to %s\n", config_path);
        return false;
    }

    fprintf(f, "{\"token\":\"%s\"}\n", token);
    fclose(f);

    // Restrict to user-only access on POSIX. Windows uses ACLs that
    // we don't model here; the file inherits the parent directory's
    // ACL which is typically already user-private under %APPDATA%.
#ifndef XR_OS_WINDOWS
    chmod(config_path, 0600);
#endif

    printf("Token saved to %s\n", config_path);
    return true;
}

bool xr_pkg_client_load_token(char **token_out) {
    if (!token_out)
        return false;

    const char *home = getenv("HOME");
    if (!home)
        return false;

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/.xray/credentials", home);

    size_t len;
    char *content = xr_file_read_all(config_path, "r", &len);
    if (!content)
        return false;

    XrJsonValue *root = xjson_parse(content, len);
    xr_free(content);
    if (!root) {
        // Migrate legacy TOML format: token = "VALUE"
        content = xr_file_read_all(config_path, "r", &len);
        if (!content)
            return false;
        const char *q1 = strchr(content, '"');
        if (q1) {
            const char *q2 = strchr(q1 + 1, '"');
            if (q2 && q2 > q1 + 1) {
                *token_out = xr_strndup(q1 + 1, (size_t) (q2 - q1 - 1));
            }
        }
        xr_free(content);
        return *token_out != NULL;
    }
    const char *val = xjson_get_string(root, "token");
    *token_out = val ? xr_strdup(val) : NULL;
    xjson_free(root);
    return *token_out != NULL;
}

#else  // !XR_HAS_NETWORK && XR_STDLIB_MODULAR

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

// Stub implementations when network is disabled
bool xr_pkg_client_init(void) {
    return false;
}
void xr_pkg_client_cleanup(void) {
}
void xr_pkg_client_set_config(const XrPkgClientConfig *config) {
    (void) config;
}
void xr_pkg_client_set_isolate(XrayIsolate *isolate) {
    (void) isolate;
}

XrPackageInfo *xr_pkg_client_get_info(const char *owner, const char *name) {
    (void) owner;
    (void) name;
    return NULL;
}
bool xr_pkg_client_get_versions(const char *owner, const char *name, char ***versions, int *count) {
    (void) owner;
    (void) name;
    (void) versions;
    (void) count;
    return false;
}
XrPkgSearchResult *xr_pkg_client_search(const char *query) {
    (void) query;
    return NULL;
}
void xr_pkg_search_result_free(XrPkgSearchResult *result) {
    (void) result;
}

bool xr_pkg_client_download(const char *owner, const char *name, const char *version,
                            const char *dest) {
    (void) owner;
    (void) name;
    (void) version;
    (void) dest;
    return false;
}
bool xr_pkg_client_install(const char *owner, const char *name, const char *version,
                           const char *dest) {
    (void) owner;
    (void) name;
    (void) version;
    (void) dest;
    return false;
}
bool xr_pkg_client_publish(const char *tarball, const char *token, const XrPkgPublishInfo *info) {
    (void) tarball;
    (void) token;
    (void) info;
    return false;
}

bool xr_pkg_client_login(char **token_out) {
    (void) token_out;
    return false;
}
bool xr_pkg_client_save_token(const char *token) {
    (void) token;
    return false;
}
bool xr_pkg_client_load_token(char **token_out) {
    (void) token_out;
    return false;
}

XrPkgResponse *xr_pkg_http_get(const char *url) {
    (void) url;
    return NULL;
}
XrPkgResponse *xr_pkg_http_post(const char *url, const char *body, const char *ct) {
    (void) url;
    (void) body;
    (void) ct;
    return NULL;
}
bool xr_pkg_http_download_file(const char *url, const char *dest) {
    (void) url;
    (void) dest;
    return false;
}
void xr_pkg_response_free(XrPkgResponse *resp) {
    (void) resp;
}

#endif  // XR_HAS_NETWORK
