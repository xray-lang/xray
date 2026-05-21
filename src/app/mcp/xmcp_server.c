/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_server.c - MCP server main loop and stdio transport
 *
 * KEY CONCEPT:
 *   Table-driven JSON-RPC 2.0 dispatch, line-delimited JSON on stdio,
 *   signal-safe shutdown (SIGTERM/SIGINT/SIGPIPE), parent process monitor,
 *   and notification sending infrastructure for log/progress messages.
 *   All diagnostic output goes to stderr (never stdout).
 */

#include "xmcp_server.h"
#include "xmcp_protocol.h"
#include "xmcp_jsonrpc.h"
#include "xmcp_tools.h"
#include "xmcp_resources.h"
#include "xmcp_prompts.h"
#include "xmcp_knowledge.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xjson.h"
#include "../cli/xcli_isolate.h"
#include "../cli/xcli_spec.h"
#include "../cli/xcli_diag.h"
#include "xray_isolate.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include "../../os/os_fd.h"

#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

#define MCP_LOG_PREFIX "[mcp] "

/* Global server pointer for signal handler (single-instance) */
static volatile XmcpServer *g_mcp_server = NULL;

/* --------------------------------------------------------------------------
 * Logging (always to stderr, never stdout)
 * -------------------------------------------------------------------------- */

static void mcp_log(XmcpServer *s, int level, const char *fmt, ...) {
    if (!s || level > s->log_level)
        return;
    const char *tags[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    const char *tag = (level >= 0 && level <= 3) ? tags[level] : "?";

    va_list ap;
    va_start(ap, fmt);

    if (s->log_file) {
        va_list ap2;
        va_copy(ap2, ap);
        fprintf(s->log_file, MCP_LOG_PREFIX "[%s] ", tag);
        vfprintf(s->log_file, fmt, ap2);
        fprintf(s->log_file, "\n");
        fflush(s->log_file);
        va_end(ap2);
    }

    fprintf(stderr, MCP_LOG_PREFIX "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

static void mcp_signal_handler(int sig) {
    (void) sig;
    if (g_mcp_server) {
        /* Safe: volatile sig_atomic_t write is async-signal-safe */
        ((XmcpServer *) g_mcp_server)->shutdown = 1;
    }
}

static void mcp_install_signals(XmcpServer *s) {
    g_mcp_server = s;
#ifdef XR_OS_WINDOWS
    signal(SIGTERM, mcp_signal_handler);
    signal(SIGINT, mcp_signal_handler);
    /* SIGPIPE does not exist on Windows */
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mcp_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    /* Ignore SIGPIPE to avoid crash on broken pipe */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
#endif
}

/* --------------------------------------------------------------------------
 * JSON-RPC response / error helpers
 * -------------------------------------------------------------------------- */

static void mcp_send_response(XmcpServer *s, XrJsonValue *id, XrJsonValue *result,
                              XrJsonValue *error) {
    XR_DCHECK(s != NULL, "mcp_send_response: NULL server");
    XrJsonValue *resp = xjson_new_object();
    XJSON_SET_STRING(resp, "jsonrpc", "2.0");

    if (id) {
        xjson_object_set(resp, "id", xjson_clone(id));
    } else {
        XJSON_SET_NULL(resp, "id");
    }

    if (error) {
        xjson_object_set(resp, "error", error);
    } else {
        xjson_object_set(resp, "result", result ? result : xjson_new_object());
    }

    size_t len = 0;
    char *json = xjson_stringify(resp, &len);
    if (json) {
        xmcp_stdio_write_message(&s->transport, json, len);
        xr_free(json);
    }
    xjson_free(resp);
}

static void mcp_send_error(XmcpServer *s, XrJsonValue *id, int code, const char *message) {
    XrJsonValue *err = xjson_new_object();
    XJSON_SET_INT(err, "code", code);
    XJSON_SET_STRING(err, "message", message);
    mcp_send_response(s, id, NULL, err);
}

/* --------------------------------------------------------------------------
 * Notification sending (public API)
 * -------------------------------------------------------------------------- */

void xmcp_send_notification(XmcpServer *server, const char *method, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_send_notification: NULL server");
    XR_DCHECK(method != NULL, "xmcp_send_notification: NULL method");
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *msg = xjson_new_object();
    XJSON_SET_STRING(msg, "jsonrpc", "2.0");
    XJSON_SET_STRING(msg, "method", method);
    if (params) {
        xjson_object_set(msg, "params", params);
    }

    size_t len = 0;
    char *json = xjson_stringify(msg, &len);
    if (json) {
        xmcp_stdio_write_message(&server->transport, json, len);
        xr_free(json);
    }
    xjson_free(msg);
}

void xmcp_send_log_notification(XmcpServer *server, const char *level, const char *message) {
    XR_DCHECK(server != NULL, "xmcp_send_log_notification: NULL server");
    XR_DCHECK(level != NULL, "xmcp_send_log_notification: NULL level");
    XR_DCHECK(message != NULL, "xmcp_send_log_notification: NULL message");
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "level", level);
    xjson_object_set(params, "data", xjson_new_string(message));
    xmcp_send_notification(server, "notifications/message", params);
}

void xmcp_send_progress_notification(XmcpServer *server, int64_t progress_token, int progress,
                                     int total) {
    XR_DCHECK(server != NULL, "xmcp_send_progress_notification: NULL server");
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_INT(params, "progressToken", progress_token);
    XJSON_SET_INT(params, "progress", progress);
    if (total > 0) {
        XJSON_SET_INT(params, "total", total);
    }
    xmcp_send_notification(server, "notifications/progress", params);
}

/* --------------------------------------------------------------------------
 * Method handlers (thin wrappers matching XmcpMethodHandler signature)
 * -------------------------------------------------------------------------- */

static XrJsonValue *handle_initialized(XmcpServer *s, XrJsonValue *params) {
    (void) params;
    if (s->lifecycle_state == XMCP_LIFECYCLE_INITIALIZE_SENT) {
        s->lifecycle_state = XMCP_LIFECYCLE_READY;
        mcp_log(s, 2, "client initialized");
    }
    return NULL; /* notification, no response */
}

static XrJsonValue *handle_ping(XmcpServer *s, XrJsonValue *params) {
    (void) s;
    (void) params;
    return xjson_new_object();
}

static XrJsonValue *handle_tools_list(XmcpServer *s, XrJsonValue *params) {
    (void) s;
    return xmcp_handle_tools_list(params);
}

static XrJsonValue *handle_tools_call(XmcpServer *s, XrJsonValue *params) {
    return xmcp_handle_tools_call(s, params);
}

static XrJsonValue *handle_resources_list(XmcpServer *s, XrJsonValue *params) {
    (void) params;
    return xmcp_handle_resources_list(s);
}

static XrJsonValue *handle_resources_read(XmcpServer *s, XrJsonValue *params) {
    return xmcp_handle_resources_read(s, params);
}

static XrJsonValue *handle_resource_templates_list(XmcpServer *s, XrJsonValue *params) {
    (void) params;
    return xmcp_handle_resource_templates_list(s);
}

static XrJsonValue *handle_prompts_list(XmcpServer *s, XrJsonValue *params) {
    (void) s;
    (void) params;
    return xmcp_handle_prompts_list();
}

static XrJsonValue *handle_prompts_get(XmcpServer *s, XrJsonValue *params) {
    return xmcp_handle_prompts_get(s, params);
}

/* --------------------------------------------------------------------------
 * Table-driven method dispatch
 * -------------------------------------------------------------------------- */

static const XmcpMethodEntry METHOD_TABLE[] = {
    /* method                       handler                notification  needs_init */
    {"initialize", xmcp_handle_initialize, false, false},
    {"notifications/initialized", handle_initialized, true, false},
    {"notifications/cancelled", NULL, true, false},
    {"ping", handle_ping, false, false},
    {"tools/list", handle_tools_list, false, true},
    {"tools/call", handle_tools_call, false, true},
    {"resources/list", handle_resources_list, false, true},
    {"resources/read", handle_resources_read, false, true},
    {"resources/templates/list", handle_resource_templates_list, false, true},
    {"prompts/list", handle_prompts_list, false, true},
    {"prompts/get", handle_prompts_get, false, true},
    {NULL, NULL, false, false}};

static void mcp_dispatch(XmcpServer *s, XrJsonValue *msg) {
    XR_DCHECK(s != NULL, "mcp_dispatch: NULL server");
    XR_DCHECK(msg != NULL, "mcp_dispatch: NULL msg");

    XmcpJsonRpcMessage req;
    XrJsonValue *error_id = NULL;
    int error_code = 0;
    const char *error_message = NULL;
    if (!xmcp_jsonrpc_validate_message(msg, &req, &error_id, &error_code, &error_message)) {
        if (!error_message)
            error_message = "Invalid Request";
        mcp_send_error(s, error_id, error_code, error_message);
        return;
    }

    const char *method = req.method;
    XrJsonValue *id = req.id;
    XrJsonValue *params = req.params;
    mcp_log(s, 3, "dispatch: %s", method);

    /* Look up method in table */
    const XmcpMethodEntry *entry = NULL;
    for (int i = 0; METHOD_TABLE[i].method != NULL; i++) {
        if (strcmp(method, METHOD_TABLE[i].method) == 0) {
            entry = &METHOD_TABLE[i];
            break;
        }
    }

    if (!entry) {
        /* Unknown method: error for requests, silently ignore notifications */
        if (!req.is_notification) {
            mcp_send_error(s, id, XMCP_ERR_METHOD_NOT_FOUND, "Method not found");
        }
        mcp_log(s, 3, "unknown method: %s (id=%s)", method, id ? "present" : "none");
        return;
    }

    if (entry->is_notification && !req.is_notification) {
        mcp_send_error(s, id, XMCP_ERR_INVALID_REQ,
                       "Invalid Request: notification method cannot have id");
        return;
    }

    if (!entry->is_notification && req.is_notification) {
        mcp_send_error(s, NULL, XMCP_ERR_INVALID_REQ,
                       "Invalid Request: request method requires id");
        return;
    }

    if (strcmp(method, "initialize") == 0 && s->lifecycle_state != XMCP_LIFECYCLE_CREATED) {
        mcp_send_error(s, id, XMCP_ERR_ALREADY_INIT, "Server already initialized");
        return;
    }

    /* Pre-init guard */
    if (entry->needs_init && s->lifecycle_state != XMCP_LIFECYCLE_READY) {
        if (id)
            mcp_send_error(s, id, XMCP_ERR_NOT_INITIALIZED, "Server not initialized");
        return;
    }

    /* Ignored notification (e.g. notifications/cancelled with NULL handler) */
    if (!entry->handler) {
        mcp_log(s, 3, "ignored notification: %s", method);
        return;
    }

    /* Call handler */
    XrJsonValue *result = entry->handler(s, params);

    /* Send response for requests (not notifications) */
    if (!entry->is_notification && id) {
        mcp_send_response(s, id, result, NULL);
    } else if (result) {
        /* Notification handler returned a value — free it */
        xjson_free(result);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

XmcpServer *xmcp_server_new(void) {
    XmcpServer *s = xr_calloc(1, sizeof(XmcpServer));
    if (!s)
        return NULL;

    if (!xmcp_stdio_init(&s->transport, xr_stdin_fd(), xr_stdout_fd(), XMCP_STDIO_MAX_LINE)) {
        xr_free(s);
        return NULL;
    }
    s->log_level = 2; /* info */

    /* Disable stdout buffering so responses reach the client immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Create parser isolate for xray_check */
    s->isolate = xr_cli_isolate_new(XR_CLI_ISOLATE_ANALYZE);
    if (!s->isolate) {
        mcp_log(s, 0, "failed to create isolate");
        xmcp_stdio_destroy(&s->transport);
        xr_free(s);
        return NULL;
    }

    /* Load knowledge base */
    s->knowledge = xmcp_knowledge_new();
    if (s->knowledge) {
        xmcp_knowledge_load(s->knowledge);
    }

    s->current_progress_token = -1;
    xmcp_registry_init(&s->registry);

    return s;
}

void xmcp_server_free(XmcpServer *s) {
    if (!s)
        return;
    if (g_mcp_server == s)
        g_mcp_server = NULL;
    if (s->knowledge)
        xmcp_knowledge_free(s->knowledge);
    if (s->isolate)
        xray_isolate_delete(s->isolate);
    if (s->log_file)
        fclose(s->log_file);
    xmcp_stdio_destroy(&s->transport);
    xr_free(s);
}

int xmcp_server_run(XmcpServer *s) {
    XR_DCHECK(s != NULL, "xmcp_server_run: NULL server");

    /* Install signal handlers for graceful shutdown */
    mcp_install_signals(s);

    mcp_log(s, 2, "MCP server starting (stdio)");

    while (!s->shutdown) {
        char *body = NULL;
        XmcpStdioReadStatus status = xmcp_stdio_read_message(&s->transport, &body);
        if (status == XMCP_STDIO_READ_EOF) {
            mcp_log(s, 2, "stdin closed, shutting down");
            break;
        }
        if (status == XMCP_STDIO_READ_TOO_LARGE) {
            mcp_log(s, 0, "input line exceeds MCP stdio limit");
            mcp_send_error(s, NULL, XMCP_ERR_INVALID_REQ, "Invalid Request: message too large");
            break;
        }
        if (status != XMCP_STDIO_READ_OK) {
            mcp_log(s, 0, "stdio read error");
            break;
        }

        XrJsonValue *msg = xjson_parse(body, strlen(body));
        xr_free(body);

        if (!msg) {
            mcp_log(s, 0, "JSON parse error");
            mcp_send_error(s, NULL, XMCP_ERR_PARSE, "Parse error");
            continue;
        }

        mcp_dispatch(s, msg);
        xjson_free(msg);
    }

    mcp_log(s, 2, "MCP server stopped");
    return 0;
}

/* --------------------------------------------------------------------------
 * CLI entry: xray mcp-server [options]
 * -------------------------------------------------------------------------- */

static int parse_log_level(const char *level) {
    if (!level)
        return 2; /* info */
    if (strcmp(level, "error") == 0)
        return 0;
    if (strcmp(level, "warn") == 0)
        return 1;
    if (strcmp(level, "info") == 0)
        return 2;
    if (strcmp(level, "debug") == 0)
        return 3;
    return 2; /* default: info */
}

XR_FUNC int cmd_mcp_server(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    const char *level_str = xr_cli_opt_string(&inv->options, "log-level", NULL);
    const char *log_file_path = xr_cli_opt_string(&inv->options, "log-file", NULL);
    int log_level = parse_log_level(level_str);

    XmcpServer *s = xmcp_server_new();
    if (!s) {
        xr_cli_error("mcp-server", "failed to create MCP server");
        return XR_CLI_EXIT_INTERNAL;
    }
    s->log_level = log_level;

    if (log_file_path) {
        s->log_file = fopen(log_file_path, "a");
        if (!s->log_file) {
            xr_cli_warn("mcp-server", "cannot open log file '%s'", log_file_path);
        }
    }

    int rc = xmcp_server_run(s);
    xmcp_server_free(s);
    return (rc != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
