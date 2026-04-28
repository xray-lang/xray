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
 *   Table-driven JSON-RPC 2.0 dispatch, Content-Length framing on stdio,
 *   signal-safe shutdown (SIGTERM/SIGINT/SIGPIPE), parent process monitor,
 *   and notification sending infrastructure for log/progress messages.
 *   All diagnostic output goes to stderr (never stdout).
 */

#include "xmcp_server.h"
#include "xmcp_protocol.h"
#include "xmcp_tools.h"
#include "xmcp_resources.h"
#include "xmcp_prompts.h"
#include "xmcp_knowledge.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xjson.h"
#include "../../base/xframing.h"
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

#define MCP_READ_BUF_INIT 4096
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
 * Blocking stdio transport
 * -------------------------------------------------------------------------- */

/* Write a JSON-RPC message with Content-Length header to stdout. */
void xmcp_write_message(const char *json, size_t len) {
    XR_DCHECK(json != NULL, "xmcp_write_message: NULL json");
    char header[64];
    int hlen = xr_frame_write_header(header, sizeof(header), len);
    XR_DCHECK(hlen > 0, "Content-Length header overflow");

    size_t total = 0;
    while (total < (size_t) hlen) {
        ssize_t n = write(xr_stdout_fd(), header + total, (size_t) hlen - total);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        total += (size_t) n;
    }
    total = 0;
    while (total < len) {
        ssize_t n = write(xr_stdout_fd(), json + total, len - total);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        total += (size_t) n;
    }
}

/* Ensure read buffer has room for `needed` more bytes. */
static bool mcp_ensure_buf(XmcpServer *s, size_t needed) {
    size_t req = s->read_len + needed + 1;
    if (s->read_cap >= req)
        return true;
    size_t new_cap = s->read_cap ? s->read_cap * 2 : MCP_READ_BUF_INIT;
    while (new_cap < req)
        new_cap *= 2;
    char *tmp = s->read_buf;
    if (!XR_REALLOC(tmp, new_cap))
        return false;
    s->read_buf = tmp;
    s->read_cap = new_cap;
    return true;
}

/* Read one complete JSON-RPC message from stdin. Caller must xr_free(). */
static char *mcp_read_message(XmcpServer *s) {
    XR_DCHECK(s != NULL, "mcp_read_message: NULL server");

    /* Accumulate bytes until a complete frame is available. */
    while (true) {
        size_t header_end = 0;
        int content_length = -1;
        XrFrameStatus fs = xr_frame_parse(s->read_buf, s->read_len, &header_end, &content_length);
        if (fs == XR_FRAME_ERROR) {
            mcp_log(s, 0, "missing or invalid Content-Length");
            return NULL;
        }
        if (fs == XR_FRAME_OK) {
            char *body = xr_malloc((size_t) content_length + 1);
            if (!body)
                return NULL;
            memcpy(body, s->read_buf + header_end, (size_t) content_length);
            body[content_length] = '\0';

            /* Slide remaining bytes down */
            size_t consumed = header_end + (size_t) content_length;
            size_t remaining = s->read_len - consumed;
            if (remaining > 0) {
                memmove(s->read_buf, s->read_buf + consumed, remaining);
            }
            s->read_len = remaining;
            if (s->read_len < s->read_cap)
                s->read_buf[s->read_len] = '\0';
            return body;
        }

        /* XR_FRAME_PARTIAL — need more data; do a blocking read. */
        if (!mcp_ensure_buf(s, 1024))
            return NULL;
        ssize_t n = read(xr_stdin_fd(), s->read_buf + s->read_len, s->read_cap - s->read_len - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        if (n == 0)
            return NULL; /* EOF */
        s->read_len += (size_t) n;
        s->read_buf[s->read_len] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * JSON-RPC response / error helpers
 * -------------------------------------------------------------------------- */

static void mcp_send_response(XrJsonValue *id, XrJsonValue *result, XrJsonValue *error) {
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
        xmcp_write_message(json, len);
        xr_free(json);
    }
    xjson_free(resp);
}

static void mcp_send_error(XrJsonValue *id, int code, const char *message) {
    XrJsonValue *err = xjson_new_object();
    XJSON_SET_INT(err, "code", code);
    XJSON_SET_STRING(err, "message", message);
    mcp_send_response(id, NULL, err);
}

/* --------------------------------------------------------------------------
 * Notification sending (public API)
 * -------------------------------------------------------------------------- */

void xmcp_send_notification(XmcpServer *server, const char *method, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_send_notification: NULL server");
    XR_DCHECK(method != NULL, "xmcp_send_notification: NULL method");
    if (!server->initialized)
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
        xmcp_write_message(json, len);
        xr_free(json);
    }
    xjson_free(msg);
}

void xmcp_send_log_notification(XmcpServer *server, const char *level, const char *message) {
    XR_DCHECK(server != NULL, "xmcp_send_log_notification: NULL server");
    XR_DCHECK(level != NULL, "xmcp_send_log_notification: NULL level");
    XR_DCHECK(message != NULL, "xmcp_send_log_notification: NULL message");
    if (!server->initialized)
        return;

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "level", level);
    xjson_object_set(params, "data", xjson_new_string(message));
    xmcp_send_notification(server, "notifications/message", params);
}

void xmcp_send_progress_notification(XmcpServer *server, int64_t progress_token, int progress,
                                     int total) {
    XR_DCHECK(server != NULL, "xmcp_send_progress_notification: NULL server");
    if (!server->initialized)
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
    s->initialized = true;
    mcp_log(s, 2, "client initialized");
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

    const char *method = xjson_get_string(msg, "method");
    XrJsonValue *id = xjson_get(msg, "id");
    XrJsonValue *params = xjson_get(msg, "params");

    if (!method) {
        if (id)
            mcp_send_error(id, XMCP_ERR_INVALID_REQ, "Invalid Request: missing method");
        return;
    }

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
        if (id) {
            mcp_send_error(id, XMCP_ERR_METHOD_NOT_FOUND, "Method not found");
        }
        mcp_log(s, 3, "unknown method: %s (id=%s)", method, id ? "present" : "none");
        return;
    }

    /* Pre-init guard */
    if (entry->needs_init && !s->initialized) {
        if (id)
            mcp_send_error(id, XMCP_ERR_NOT_INITIALIZED, "Server not initialized");
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
        mcp_send_response(id, result, NULL);
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

    s->read_buf = xr_malloc(MCP_READ_BUF_INIT);
    if (!s->read_buf) {
        xr_free(s);
        return NULL;
    }
    s->read_cap = MCP_READ_BUF_INIT;
    s->log_level = 2; /* info */

    /* Disable stdout buffering so responses reach the client immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Create parser isolate for xray_check */
    s->isolate = xr_cli_isolate_new(XR_CLI_ISOLATE_ANALYZE);
    if (!s->isolate) {
        mcp_log(s, 0, "failed to create isolate");
        xr_free(s->read_buf);
        xr_free(s);
        return NULL;
    }

    /* Load knowledge base */
    s->knowledge = xmcp_knowledge_new();
    if (s->knowledge) {
        xmcp_knowledge_load(s->knowledge);
    }

    s->current_progress_token = -1;

    /* Set feature flags for capability inference */
    s->has_tools = true;     /* 7 built-in tools */
    s->has_resources = true; /* 3 static resources */
    s->has_prompts = true;   /* 5 built-in prompts */

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
    xr_free(s->read_buf);
    xr_free(s);
}

int xmcp_server_run(XmcpServer *s) {
    XR_DCHECK(s != NULL, "xmcp_server_run: NULL server");

    /* Install signal handlers for graceful shutdown */
    mcp_install_signals(s);

    mcp_log(s, 2, "MCP server starting (stdio)");

    while (!s->shutdown) {
        char *body = mcp_read_message(s);
        if (!body) {
            mcp_log(s, 2, "stdin closed, shutting down");
            break;
        }

        XrJsonValue *msg = xjson_parse(body, strlen(body));
        xr_free(body);

        if (!msg) {
            mcp_log(s, 0, "JSON parse error");
            mcp_send_error(NULL, XMCP_ERR_PARSE, "Parse error");
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
