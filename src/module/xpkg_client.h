/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpkg_client.h - Package registry client
 *
 * KEY CONCEPT:
 *   HTTP client for pkg.xray-lang.org registry.
 *   Supports search, download, publish, and version queries.
 *
 * API ENDPOINTS:
 *   GET  /api/packages/{owner}/{name}              - Package info
 *   GET  /api/packages/{owner}/{name}/versions     - Version list
 *   GET  /api/packages/{owner}/{name}/{version}    - Download
 *   POST /api/packages                             - Publish
 *   GET  /api/search?q={query}                     - Search
 */

#ifndef XPKG_CLIENT_H
#define XPKG_CLIENT_H

#include "xresolver.h"
#include <stdbool.h>
#include <stddef.h>

#define PKG_REGISTRY_URL "https://pkg.xray-lang.org"

typedef struct XrPkgClientConfig {
    const char *registry_url;
    const char *auth_token;     // Optional
    int timeout_ms;
    bool verbose;
} XrPkgClientConfig;

typedef struct XrPkgResponse {
    int status_code;
    char *body;
    size_t body_len;
    char *error;
    bool success;
} XrPkgResponse;

typedef struct XrPkgSearchResult {
    char **names;
    char **descriptions;
    int count;
} XrPkgSearchResult;

/* ========== Client API ========== */

// Must call before using other APIs
XR_FUNC bool xr_pkg_client_init(void);
XR_FUNC void xr_pkg_client_cleanup(void);
XR_FUNC void xr_pkg_client_set_config(const XrPkgClientConfig *config);

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"

// Set isolate for HTTP requests (required before making any requests)
XR_FUNC void xr_pkg_client_set_isolate(XrayIsolate *isolate);

/* ========== Package Info API ========== */

XR_FUNC XrPackageInfo* xr_pkg_client_get_info(const char *owner, const char *name);
XR_FUNC bool xr_pkg_client_get_versions(const char *owner, const char *name,
                                char ***versions, int *count);
XR_FUNC XrPkgSearchResult* xr_pkg_client_search(const char *query);
XR_FUNC void xr_pkg_search_result_free(XrPkgSearchResult *result);

/* ========== Download API ========== */

XR_FUNC bool xr_pkg_client_download(const char *owner, const char *name,
                            const char *version, const char *dest_dir);
XR_FUNC bool xr_pkg_client_install(const char *owner, const char *name,
                           const char *version, const char *dest_dir);

/* ========== Publish API ========== */

typedef struct XrPkgPublishInfo {
    const char *name;        // e.g. "xray/sqlite"
    const char *version;     // e.g. "1.0.0"
    const char *description; // optional
    const char *license;     // optional
} XrPkgPublishInfo;

XR_FUNC bool xr_pkg_client_publish(const char *tarball_path, const char *auth_token,
                                    const XrPkgPublishInfo *info);

/* ========== Auth API ========== */

// Login via browser OAuth flow, caller must free token
XR_FUNC bool xr_pkg_client_login(char **token_out);
XR_FUNC bool xr_pkg_client_save_token(const char *token);
XR_FUNC bool xr_pkg_client_load_token(char **token_out);

/* ========== Low-level HTTP API ========== */

XR_FUNC XrPkgResponse* xr_pkg_http_get(const char *url);
XR_FUNC XrPkgResponse* xr_pkg_http_post(const char *url, const char *body,
                                const char *content_type);
XR_FUNC bool xr_pkg_http_download_file(const char *url, const char *dest_path);
XR_FUNC void xr_pkg_response_free(XrPkgResponse *response);

#endif // XPKG_CLIENT_H
