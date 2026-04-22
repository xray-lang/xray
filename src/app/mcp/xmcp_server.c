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
 *   Blocking read on stdin, JSON-RPC 2.0 dispatch to tool/resource
 *   handlers, write responses to stdout with Content-Length framing.
 *   All diagnostic/log output goes to stderr (never stdout).
 */

#include "xmcp_server.h"
#include "xmcp_protocol.h"
#include "xmcp_tools.h"
#include "xmcp_resources.h"
#include "xmcp_knowledge.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../lsp/xlsp_json.h"
#include "../cli/xcli_utils.h"
#include "xray_isolate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#endif

#define MCP_READ_BUF_INIT 4096
#define MCP_LOG_PREFIX    "[mcp] "

/* --------------------------------------------------------------------------
 * Logging (always to stderr, never stdout)
 * -------------------------------------------------------------------------- */

static void mcp_log(XmcpServer *s, int level, const char *fmt, ...) {
    if (!s || level > s->log_level) return;
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
 * Blocking stdio transport
 * -------------------------------------------------------------------------- */

/* Write a JSON-RPC message with Content-Length header to stdout. */
static void mcp_write(const char *json, size_t len) {
    XR_DCHECK(json != NULL, "mcp_write: NULL json");
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", len);
    XR_DCHECK(hlen > 0 && hlen < (int)sizeof(header),
              "Content-Length header overflow");

    size_t total = 0;
    while (total < (size_t)hlen) {
        ssize_t n = write(STDOUT_FILENO, header + total, (size_t)hlen - total);
        if (n <= 0) { if (errno == EINTR) continue; return; }
        total += (size_t)n;
    }
    total = 0;
    while (total < len) {
        ssize_t n = write(STDOUT_FILENO, json + total, len - total);
        if (n <= 0) { if (errno == EINTR) continue; return; }
        total += (size_t)n;
    }
}

/* Ensure read buffer has room for `needed` more bytes. */
static bool mcp_ensure_buf(XmcpServer *s, size_t needed) {
    size_t req = s->read_len + needed + 1;
    if (s->read_cap >= req) return true;
    size_t new_cap = s->read_cap ? s->read_cap * 2 : MCP_READ_BUF_INIT;
    while (new_cap < req) new_cap *= 2;
    char *tmp = s->read_buf;
    if (!XR_REALLOC(tmp, new_cap)) return false;
    s->read_buf = tmp;
    s->read_cap = new_cap;
    return true;
}

/* Read exactly `count` bytes from stdin (blocking). Returns false on EOF. */
static bool mcp_read_exact(XmcpServer *s, size_t count) {
    if (!mcp_ensure_buf(s, count)) return false;
    size_t got = 0;
    while (got < count) {
        ssize_t n = read(STDIN_FILENO,
                         s->read_buf + s->read_len + got,
                         count - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; /* EOF */
        got += (size_t)n;
    }
    s->read_len += count;
    s->read_buf[s->read_len] = '\0';
    return true;
}

/* Read one line from stdin (up to \n). Returns false on EOF. */
static bool mcp_read_line(XmcpServer *s, char *line, size_t cap) {
    (void)s;
    size_t pos = 0;
    while (pos < cap - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        line[pos++] = c;
        if (c == '\n') break;
    }
    line[pos] = '\0';
    return true;
}

/* Read one complete JSON-RPC message from stdin. Caller must xr_free(). */
static char *mcp_read_message(XmcpServer *s) {
    XR_DCHECK(s != NULL, "mcp_read_message: NULL server");
    /* Parse headers until empty line */
    int content_length = -1;
    char line[1024];

    while (true) {
        if (!mcp_read_line(s, line, sizeof(line))) return NULL;
        /* Empty line (just \r\n or \n) marks end of headers */
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;

        /* Parse Content-Length */
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
    }

    if (content_length <= 0) {
        mcp_log(s, 0, "missing or invalid Content-Length");
        return NULL;
    }

    /* Read body */
    s->read_len = 0;
    if (!mcp_read_exact(s, (size_t)content_length)) return NULL;

    char *body = xr_malloc((size_t)content_length + 1);
    if (!body) return NULL;
    memcpy(body, s->read_buf, (size_t)content_length);
    body[content_length] = '\0';
    s->read_len = 0;
    return body;
}

/* --------------------------------------------------------------------------
 * JSON-RPC dispatch
 * -------------------------------------------------------------------------- */

/* Send a JSON-RPC response (result or error). */
static void mcp_send_response(XrJsonValue *id, XrJsonValue *result,
                               XrJsonValue *error) {
    XrJsonValue *resp = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(resp, "jsonrpc", "2.0");

    if (id) {
        xlsp_json_object_set(resp, "id", xlsp_json_clone(id));
    } else {
        XLSP_JSON_SET_NULL(resp, "id");
    }

    if (error) {
        xlsp_json_object_set(resp, "error", error);
    } else {
        xlsp_json_object_set(resp, "result", result ? result : xlsp_json_new_object());
    }

    size_t len = 0;
    char *json = xlsp_json_stringify(resp, &len);
    if (json) {
        mcp_write(json, len);
        xr_free(json);
    }
    xlsp_json_free(resp);
}

/* Send a JSON-RPC error response. */
static void mcp_send_error(XrJsonValue *id, int code, const char *message) {
    XrJsonValue *err = xlsp_json_new_object();
    XLSP_JSON_SET_INT(err, "code", code);
    XLSP_JSON_SET_STRING(err, "message", message);
    mcp_send_response(id, NULL, err);
}

/* Dispatch a single JSON-RPC request. */
static void mcp_dispatch(XmcpServer *s, XrJsonValue *msg) {
    XR_DCHECK(s != NULL, "mcp_dispatch: NULL server");
    XR_DCHECK(msg != NULL, "mcp_dispatch: NULL msg");

    const char *method = xlsp_json_get_string(msg, "method");
    XrJsonValue *id = xlsp_json_get(msg, "id");
    XrJsonValue *params = xlsp_json_get(msg, "params");

    if (!method) {
        if (id) mcp_send_error(id, -32600, "Invalid Request: missing method");
        return;
    }

    mcp_log(s, 3, "dispatch: %s", method);

    /* --- Lifecycle --- */
    if (strcmp(method, "initialize") == 0) {
        XrJsonValue *result = xmcp_handle_initialize(s, params);
        mcp_send_response(id, result, NULL);
        return;
    }

    if (strcmp(method, "notifications/initialized") == 0) {
        s->initialized = true;
        mcp_log(s, 2, "client initialized");
        return; /* notification, no response */
    }

    if (strcmp(method, "ping") == 0) {
        mcp_send_response(id, xlsp_json_new_object(), NULL);
        return;
    }

    /* Reject requests before initialization (except initialize) */
    if (!s->initialized) {
        if (id) mcp_send_error(id, -32002, "Server not initialized");
        return;
    }

    /* --- Tools --- */
    if (strcmp(method, "tools/list") == 0) {
        XrJsonValue *result = xmcp_handle_tools_list();
        mcp_send_response(id, result, NULL);
        return;
    }

    if (strcmp(method, "tools/call") == 0) {
        XrJsonValue *result = xmcp_handle_tools_call(s, params);
        mcp_send_response(id, result, NULL);
        return;
    }

    /* --- Resources --- */
    if (strcmp(method, "resources/list") == 0) {
        XrJsonValue *result = xmcp_handle_resources_list(s);
        mcp_send_response(id, result, NULL);
        return;
    }

    if (strcmp(method, "resources/read") == 0) {
        XrJsonValue *result = xmcp_handle_resources_read(s, params);
        mcp_send_response(id, result, NULL);
        return;
    }

    /* Unknown method */
    if (id) {
        mcp_send_error(id, -32601, "Method not found");
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

XmcpServer *xmcp_server_new(void) {
    XmcpServer *s = xr_calloc(1, sizeof(XmcpServer));
    if (!s) return NULL;

    s->read_buf = xr_malloc(MCP_READ_BUF_INIT);
    if (!s->read_buf) { xr_free(s); return NULL; }
    s->read_cap = MCP_READ_BUF_INIT;
    s->log_level = 2; /* info */

    /* Disable stdout buffering so responses reach the client immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Create parser isolate for xray_check */
    s->isolate = cli_create_isolate();
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

    return s;
}

void xmcp_server_free(XmcpServer *s) {
    if (!s) return;
    if (s->knowledge) xmcp_knowledge_free(s->knowledge);
    if (s->isolate)   xray_isolate_delete(s->isolate);
    if (s->log_file)  fclose(s->log_file);
    xr_free(s->read_buf);
    xr_free(s);
}

int xmcp_server_run(XmcpServer *s) {
    XR_DCHECK(s != NULL, "xmcp_server_run: NULL server");
    mcp_log(s, 2, "MCP server starting (stdio)");

    while (!s->shutdown) {
        char *body = mcp_read_message(s);
        if (!body) {
            mcp_log(s, 2, "stdin closed, shutting down");
            break;
        }

        XrJsonValue *msg = xlsp_json_parse(body, strlen(body));
        xr_free(body);

        if (!msg) {
            mcp_log(s, 0, "JSON parse error");
            mcp_send_error(NULL, -32700, "Parse error");
            continue;
        }

        mcp_dispatch(s, msg);
        xlsp_json_free(msg);
    }

    mcp_log(s, 2, "MCP server stopped");
    return 0;
}

/* --------------------------------------------------------------------------
 * CLI entry: xray mcp-server [options]
 * -------------------------------------------------------------------------- */

static struct option mcp_long_options[] = {
    {"log-level", required_argument, 0, 'l'},
    {"log-file",  required_argument, 0, 'f'},
    {"help",      no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

static void print_mcp_help(void) {
    fprintf(stderr, "Usage: xray mcp-server [options]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Start the MCP (Model Context Protocol) server for AI assistants.\n");
    fprintf(stderr, "Communicates via JSON-RPC over stdio.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --log-level <level>  Log level: error, warn, info, debug (default: info)\n");
    fprintf(stderr, "  --log-file <path>    Log to file (in addition to stderr)\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Configuration (in IDE):\n");
    fprintf(stderr, "  {\n");
    fprintf(stderr, "    \"mcpServers\": {\n");
    fprintf(stderr, "      \"xray\": { \"command\": \"xray\", \"args\": [\"mcp-server\"] }\n");
    fprintf(stderr, "    }\n");
    fprintf(stderr, "  }\n");
}

int cmd_mcp_server(int argc, char **argv) {
    int log_level = 2;
    const char *log_file_path = NULL;

    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "l:f:h", mcp_long_options, NULL)) != -1) {
        switch (opt) {
        case 'l':
            if (strcmp(optarg, "error") == 0)      log_level = 0;
            else if (strcmp(optarg, "warn") == 0)   log_level = 1;
            else if (strcmp(optarg, "info") == 0)   log_level = 2;
            else if (strcmp(optarg, "debug") == 0)  log_level = 3;
            break;
        case 'f':
            log_file_path = optarg;
            break;
        case 'h':
            print_mcp_help();
            return 0;
        default:
            print_mcp_help();
            return 1;
        }
    }

    XmcpServer *s = xmcp_server_new();
    if (!s) {
        fprintf(stderr, "Error: failed to create MCP server\n");
        return 1;
    }
    s->log_level = log_level;

    if (log_file_path) {
        s->log_file = fopen(log_file_path, "a");
        if (!s->log_file) {
            fprintf(stderr, "Warning: cannot open log file '%s'\n", log_file_path);
        }
    }

    int rc = xmcp_server_run(s);
    xmcp_server_free(s);
    return rc;
}
