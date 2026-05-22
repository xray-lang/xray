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
#include "../../api/xisolate_profile.h"
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

void xmcp_send_progress_notification(XmcpServer *server, const XrJsonValue *progress_token,
                                     int64_t progress, int64_t total) {
    XR_DCHECK(server != NULL, "xmcp_send_progress_notification: NULL server");
    if (!progress_token)
        return;
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "progressToken", xjson_clone(progress_token));
    XJSON_SET_INT(params, "progress", progress);
    if (total > 0) {
        XJSON_SET_INT(params, "total", total);
    }
    xmcp_send_notification(server, "notifications/progress", params);
}

/* --------------------------------------------------------------------------
 * Method handlers (thin wrappers matching XmcpMethodHandler signature)
 * -------------------------------------------------------------------------- */

static XrJsonValue *handle_initialized(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    (void) params;
    (void) error;
    if (s->lifecycle_state == XMCP_LIFECYCLE_INITIALIZE_SENT) {
        s->lifecycle_state = XMCP_LIFECYCLE_READY;
        mcp_log(s, 2, "client initialized");
    }
    return NULL; /* notification, no response */
}

static XrJsonValue *handle_cancelled(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    (void) params;
    (void) error;
    /* Sequential dispatch: cancellation cannot preempt an in-flight request, so
     * this notification is a no-op. Logged at debug level so operators can see
     * that the message was observed but intentionally ignored. */
    mcp_log(s, 3, "notifications/cancelled: no-op (sequential dispatch)");
    return NULL;
}

static XrJsonValue *handle_ping(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    (void) s;
    (void) params;
    (void) error;
    return xjson_new_object();
}

static XrJsonValue *handle_tools_list(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    return xmcp_handle_tools_list(s, params, error);
}

static XrJsonValue *handle_tools_call(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    return xmcp_handle_tools_call(s, params, error);
}

static XrJsonValue *handle_resources_list(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    (void) params;
    return xmcp_handle_resources_list(s, error);
}

static XrJsonValue *handle_resources_read(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    return xmcp_handle_resources_read(s, params, error);
}

static XrJsonValue *handle_resource_templates_list(XmcpServer *s, XrJsonValue *params,
                                                   XmcpRpcError *error) {
    (void) params;
    return xmcp_handle_resource_templates_list(s, error);
}

static XrJsonValue *handle_prompts_list(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    (void) params;
    return xmcp_handle_prompts_list(s, error);
}

static XrJsonValue *handle_prompts_get(XmcpServer *s, XrJsonValue *params, XmcpRpcError *error) {
    return xmcp_handle_prompts_get(s, params, error);
}

/* --------------------------------------------------------------------------
 * Table-driven method dispatch
 * -------------------------------------------------------------------------- */

static const XmcpMethodEntry METHOD_TABLE[] = {
    /* method                       handler                            notif  required_state */
    {"initialize", xmcp_handle_initialize, false, XMCP_LC_MUST_BE_CREATED},
    {"notifications/initialized", handle_initialized, true, XMCP_LC_ANY},
    {"notifications/cancelled", handle_cancelled, true, XMCP_LC_ANY},
    {"ping", handle_ping, false, XMCP_LC_ANY},
    {"tools/list", handle_tools_list, false, XMCP_LC_MUST_BE_READY},
    {"tools/call", handle_tools_call, false, XMCP_LC_MUST_BE_READY},
    {"resources/list", handle_resources_list, false, XMCP_LC_MUST_BE_READY},
    {"resources/read", handle_resources_read, false, XMCP_LC_MUST_BE_READY},
    {"resources/templates/list", handle_resource_templates_list, false, XMCP_LC_MUST_BE_READY},
    {"prompts/list", handle_prompts_list, false, XMCP_LC_MUST_BE_READY},
    {"prompts/get", handle_prompts_get, false, XMCP_LC_MUST_BE_READY},
    {NULL, NULL, false, XMCP_LC_ANY}};

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

    /* Lifecycle precondition. Single switch keeps every "may I run now?"
     * decision in the dispatcher table; method handlers stay stateless. */
    switch (entry->required_state) {
        case XMCP_LC_ANY:
            break;
        case XMCP_LC_MUST_BE_CREATED:
            if (s->lifecycle_state != XMCP_LIFECYCLE_CREATED) {
                mcp_send_error(s, id, XMCP_ERR_ALREADY_INIT, "Server already initialized");
                return;
            }
            break;
        case XMCP_LC_MUST_BE_READY:
            if (s->lifecycle_state != XMCP_LIFECYCLE_READY) {
                if (id)
                    mcp_send_error(s, id, XMCP_ERR_NOT_INITIALIZED, "Server not initialized");
                return;
            }
            break;
    }

    /* Call handler */
    XmcpRpcError method_err = {.code = 0};
    XrJsonValue *result = entry->handler(s, params, &method_err);

    if (method_err.code != 0) {
        /* Protocol-level failure: emit JSON-RPC error for requests, swallow for
         * notifications so misbehaving clients cannot derail the server. */
        if (!entry->is_notification && id) {
            mcp_send_error(s, id, method_err.code, method_err.message);
        } else {
            mcp_log(s, 1, "notification %s produced rpc error %d: %s", method, method_err.code,
                    method_err.message);
        }
        if (result)
            xjson_free(result);
        return;
    }

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

XR_FUNC void xmcp_server_options_default(XmcpServerOptions *options) {
    XR_DCHECK(options != NULL, "xmcp_server_options_default: NULL options");
    options->enable_runner = false;
}

XmcpServer *xmcp_server_new(const XmcpServerOptions *options) {
    XR_DCHECK(options != NULL, "xmcp_server_new: NULL options");

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

    /* Create analyzer isolate for MCP code analysis tools */
    s->isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
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

    XmcpRegistryOptions registry_options;
    xmcp_registry_options_default(&registry_options);
    registry_options.enable_runner = options->enable_runner;
    xmcp_registry_init(&s->registry, &registry_options);

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
        size_t body_len = 0;
        XmcpStdioReadStatus status = xmcp_stdio_read_message(&s->transport, &body, &body_len);
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

        XrJsonValue *msg = xjson_parse(body, body_len);
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
