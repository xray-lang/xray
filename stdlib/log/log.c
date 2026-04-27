/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * log.c - Structured logging module implementation
 *
 * KEY CONCEPT:
 *   High-performance structured logging with async write support.
 *   Uses a mutex-based ring buffer for async mode to minimize latency.
 */

#include "log.h"
#include "../common.h"
#include "../ctxbuf.h"
#include "../stdlib_cache.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xutf8.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/gc/xalloc_unified.h"
#include "../../src/runtime/object/xnative_type.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/base/xthread.h"
#include <stdarg.h>

/* ========== Async Write Buffer ========== */

#define ASYNC_QUEUE_SIZE 256

typedef struct {
    char *entries[ASYNC_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    xr_mutex_t mutex;
    xr_cond_t not_empty;
    xr_cond_t not_full;  // legacy, no longer used by producers
    xr_cond_t drained;   // signaled when queue becomes empty
    xr_thread_t thread;
    bool running;
    FILE *output;
    // Tally of messages the producer had to discard because the ring was
    // full. A high-throughput writer would otherwise deadlock the
    // producing coroutine on cond_wait; dropping-newest keeps latency
    // bounded while preserving earlier context. The background thread
    // emits a synthetic "[log] dropped=N" record whenever this counter
    // has grown since the last flush.
    size_t dropped;
    size_t dropped_reported;
} AsyncLogQueue;

/* ========== Per-Isolate Log State ========== */

// All mutable log state lives here; one instance per XrayIsolate, stored
// in XrStdlibCache::log_state. This eliminates the former process-global
// g_default_logger / g_async_queue / g_default_logger_mutex.
typedef struct XrLogState {
    XrLogger default_logger;
    xr_mutex_t mutex;            // protects setLevel/setFormat/setOutput
    AsyncLogQueue *async_queue;  // NULL until enableAsync(true)
    bool async_initialized;
} XrLogState;

// Destroy callback invoked from xr_stdlib_cache_free (via function pointer).
static void log_state_destroy(void *opaque);

// Retrieve (and lazily create) the per-isolate log state.
static XrLogState *log_state_get(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "log_state_get: isolate must not be NULL");
    XrStdlibCache *cache = xr_stdlib_cache_get(X);
    if (cache->log_state)
        return (XrLogState *) cache->log_state;

    XrLogState *s = (XrLogState *) xr_malloc(sizeof(XrLogState));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));

    s->default_logger.level = XR_LOG_INFO;
    s->default_logger.format = XR_LOG_FORMAT_TEXT;
    s->default_logger.output = stderr;
    xr_mutex_init(&s->mutex);

    cache->log_state = s;
    cache->log_state_cleanup = log_state_destroy;
    return s;
}

// Convenience: return the isolate's default logger.
static XrLogger *log_default(XrayIsolate *X) {
    XrLogState *s = log_state_get(X);
    return s ? &s->default_logger : NULL;
}

/* ========== Async Write Implementation ========== */

// Background thread function (arg is the owning AsyncLogQueue*)
static void *async_log_thread(void *arg) {
    AsyncLogQueue *q = (AsyncLogQueue *) arg;

    while (1) {
        xr_mutex_lock(&q->mutex);

        // Wait for queue not empty or stop signal
        while (q->count == 0 && q->running) {
            xr_cond_wait(&q->not_empty, &q->mutex);
        }

        // Check if should exit
        if (!q->running && q->count == 0) {
            xr_mutex_unlock(&q->mutex);
            break;
        }

        // Dequeue one entry
        char *entry = q->entries[q->head];
        q->entries[q->head] = NULL;
        q->head = (q->head + 1) % ASYNC_QUEUE_SIZE;
        q->count--;

        // Snapshot the dropped counter so we can emit a synthetic marker
        // outside the mutex; we only report the new deltas so the user
        // sees a running total grow naturally instead of repeating.
        size_t drop_now = q->dropped;
        size_t drop_reported = q->dropped_reported;
        if (drop_now > drop_reported) {
            q->dropped_reported = drop_now;
        }

        // Signal queue has space (and drained if empty)
        xr_cond_signal(&q->not_full);
        if (q->count == 0) {
            xr_cond_broadcast(&q->drained);
        }
        xr_mutex_unlock(&q->mutex);

        FILE *out = q->output ? q->output : stderr;

        // Write log entry
        if (entry) {
            fputs(entry, out);
            xr_free(entry);
        }
        // Emit dropped-marker once per drain cycle when new drops happened.
        if (drop_now > drop_reported) {
            fprintf(out, "[log] dropped=%zu (async queue full)\n", drop_now - drop_reported);
        }
        fflush(out);
    }

    return NULL;
}

// Initialize async queue on the given log state.
static void async_queue_init(XrLogState *ls, FILE *output) {
    XR_DCHECK(ls != NULL, "async_queue_init: log state must not be NULL");
    if (ls->async_initialized)
        return;

    AsyncLogQueue *q = (AsyncLogQueue *) xr_malloc(sizeof(AsyncLogQueue));
    if (!q)
        return;
    memset(q, 0, sizeof(AsyncLogQueue));

    xr_mutex_init(&q->mutex);
    xr_cond_init(&q->not_empty);
    xr_cond_init(&q->not_full);
    xr_cond_init(&q->drained);

    q->output = output;
    q->running = true;

    xr_thread_create(&q->thread, async_log_thread, q);
    ls->async_queue = q;
    ls->async_initialized = true;
}

// Stop async queue.
static void async_queue_stop(XrLogState *ls) {
    if (!ls->async_initialized || !ls->async_queue)
        return;

    AsyncLogQueue *q = ls->async_queue;

    xr_mutex_lock(&q->mutex);
    q->running = false;
    xr_cond_signal(&q->not_empty);
    xr_mutex_unlock(&q->mutex);

    xr_thread_join(q->thread, NULL);

    // Clean up remaining entries
    for (int i = 0; i < ASYNC_QUEUE_SIZE; i++) {
        if (q->entries[i]) {
            xr_free(q->entries[i]);
            q->entries[i] = NULL;
        }
    }

    xr_mutex_destroy(&q->mutex);
    xr_cond_destroy(&q->not_empty);
    xr_cond_destroy(&q->not_full);
    xr_cond_destroy(&q->drained);

    xr_free(q);
    ls->async_queue = NULL;
    ls->async_initialized = false;
}

// Write log entry asynchronously (takes ownership of msg, caller must not free).
//
// Discard strategy: if the ring buffer is full we drop the *new* record
// and increment a counter instead of blocking the producer. Rationale:
//   - A coroutine hitting log.info() in a tight loop must never stall
//     on xr_cond_wait -- that would pin a worker thread and cascade
//     into latency across unrelated tasks.
//   - Dropping newest preserves the earlier context most useful for
//     postmortem analysis; the writer thread later emits a synthetic
//     "[log] dropped=N" marker so the user knows messages were lost.
static void async_log_write(XrLogState *ls, char *msg) {
    if (!ls->async_initialized || !ls->async_queue) {
        xr_free(msg);
        return;
    }

    AsyncLogQueue *q = ls->async_queue;

    xr_mutex_lock(&q->mutex);

    if (!q->running) {
        xr_mutex_unlock(&q->mutex);
        xr_free(msg);
        return;
    }

    if (q->count >= ASYNC_QUEUE_SIZE) {
        // Ring full: drop newest, bump counter, return immediately.
        q->dropped++;
        xr_mutex_unlock(&q->mutex);
        xr_free(msg);
        return;
    }

    // Enqueue (ownership transferred, no strdup needed)
    q->entries[q->tail] = msg;
    q->tail = (q->tail + 1) % ASYNC_QUEUE_SIZE;
    q->count++;

    // Signal background thread
    xr_cond_signal(&q->not_empty);
    xr_mutex_unlock(&q->mutex);
}

// Flush async queue (blocks until complete)
static void async_queue_flush(XrLogState *ls) {
    if (!ls->async_initialized || !ls->async_queue)
        return;

    AsyncLogQueue *q = ls->async_queue;

    // Wait for queue to drain using condvar (no busy-wait)
    xr_mutex_lock(&q->mutex);
    while (q->count > 0 && q->running) {
        xr_cond_wait(&q->drained, &q->mutex);
    }
    xr_mutex_unlock(&q->mutex);
}

/* ========== Dynamic Buffer ========== */

// Thin aliases over the shared XrCtxBuf (see stdlib/ctxbuf.h). Keeping the
// legacy names avoids churn at the many call sites below while delegating
// all allocation, growth, and OOM handling to the shared implementation.
typedef XrCtxBuf CtxBuf;

#define ctxbuf_init(b, cap) xr_ctxbuf_init((b), (size_t) (cap))
#define ctxbuf_append(b, s, slen) xr_ctxbuf_append((b), (s), (size_t) (slen))
#define ctxbuf_putc(b, c) xr_ctxbuf_putc((b), (c))
#define ctxbuf_printf(b, ...) xr_ctxbuf_appendf((b), __VA_ARGS__)

/* ========== Helper Functions ========== */

static const char *xr_log_level_name(XrLogLevel level) {
    switch (level) {
        case XR_LOG_DEBUG:
            return "DEBUG";
        case XR_LOG_INFO:
            return "INFO";
        case XR_LOG_WARN:
            return "WARN";
        case XR_LOG_ERROR:
            return "ERROR";
        case XR_LOG_FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

static XrLogLevel xr_log_level_parse(const char *name) {
    if (strcasecmp(name, "debug") == 0)
        return XR_LOG_DEBUG;
    if (strcasecmp(name, "info") == 0)
        return XR_LOG_INFO;
    if (strcasecmp(name, "warn") == 0)
        return XR_LOG_WARN;
    if (strcasecmp(name, "warning") == 0)
        return XR_LOG_WARN;
    if (strcasecmp(name, "error") == 0)
        return XR_LOG_ERROR;
    if (strcasecmp(name, "fatal") == 0)
        return XR_LOG_FATAL;
    return XR_LOG_INFO;  // Default
}

// Get ISO 8601 timestamp
static void get_timestamp(char *buf, size_t size) {
    uint64_t ns = xr_time_realtime_ns();
    time_t sec = (time_t) (ns / 1000000000ULL);
    int ms = (int) ((ns % 1000000000ULL) / 1000000ULL);

    struct tm tm;
    localtime_r(&sec, &tm);

    // Format: 2024-12-14T22:45:00.123
    int len = (int) strftime(buf, size, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + len, size - len, ".%03d", ms);
}

// Escape JSON string into CtxBuf.
//
// RFC 8259 §8.1 requires the payload to be valid UTF-8; any high-bit
// byte sequence that is *not* a well-formed UTF-8 scalar gets replaced
// by U+FFFD (the Unicode replacement character, 3 bytes in UTF-8).
// This avoids poisoning downstream strict parsers when an upstream
// caller hands us arbitrary bytes (e.g. a payload decoded from
// latin-1 or a corrupted network frame).
static void write_json_string_buf(CtxBuf *b, const char *str) {
    ctxbuf_putc(b, '"');
    size_t len = strlen(str);
    const char *p = str;
    const char *end = str + len;
    while (p < end) {
        unsigned char c = (unsigned char) *p;
        if (c < 0x80) {
            // Fast path: ASCII.
            switch (c) {
                case '"':
                    ctxbuf_append(b, "\\\"", 2);
                    break;
                case '\\':
                    ctxbuf_append(b, "\\\\", 2);
                    break;
                case '\n':
                    ctxbuf_append(b, "\\n", 2);
                    break;
                case '\r':
                    ctxbuf_append(b, "\\r", 2);
                    break;
                case '\t':
                    ctxbuf_append(b, "\\t", 2);
                    break;
                default:
                    if (c < 0x20) {
                        char esc[8];
                        int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                        ctxbuf_append(b, esc, n);
                    } else {
                        ctxbuf_putc(b, (char) c);
                    }
            }
            p++;
        } else {
            uint32_t cp = 0;
            int size = xr_utf8_decode(p, (size_t) (end - p), &cp);
            if (size <= 0 || cp == XR_UNICODE_INVALID) {
                // Invalid byte: emit U+FFFD (EF BF BD) and skip one byte.
                ctxbuf_append(b, "\xEF\xBF\xBD", 3);
                p++;
            } else {
                // Valid UTF-8 scalar: copy bytes verbatim.
                ctxbuf_append(b, p, size);
                p += size;
            }
        }
    }
    ctxbuf_putc(b, '"');
}

// Convert XrValue to string.
//
// Float formatting uses %.17g so that the string representation round-trips
// exactly back to the original double value (the minimum guaranteed
// precision for IEEE-754 binary64). The previous %.6g truncated meaningful
// digits for sensor/metric payloads.
static const char *value_to_string(XrValue val, char *buf, size_t size) {
    if (XR_IS_STRING(val)) {
        return XR_STRING_CHARS(XR_TO_STRING(val));
    } else if (XR_IS_INT(val)) {
        snprintf(buf, size, "%lld", (long long) XR_TO_INT(val));
        return buf;
    } else if (XR_IS_FLOAT(val)) {
        snprintf(buf, size, "%.17g", XR_TO_FLOAT(val));
        return buf;
    } else if (XR_IS_BOOL(val)) {
        return XR_TO_BOOL(val) ? "true" : "false";
    } else if (XR_IS_NULL(val)) {
        return "null";
    }
    return "<object>";
}

// Check if string needs quoting (for Text format)
static bool needs_quoting(const char *str) {
    for (const char *p = str; *p; p++) {
        if (*p == ' ' || *p == '"' || *p == '=' || *p == '\n' || *p == '\t') {
            return true;
        }
    }
    return false;
}

/* ========== Core Log Output ========== */

// Get filename without path
static const char *get_filename(const char *path) {
    if (!path)
        return NULL;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void xr_log_write_ex(XrLogState *ls, XrLogger *logger, XrLogLevel level, const char *msg,
                            XrValue *attrs, int nattrs, const char *source_file, int source_line) {
    XR_DCHECK(msg != NULL, "xr_log_write_ex: NULL msg");
    XR_DCHECK(logger != NULL, "xr_log_write_ex: NULL logger");

    // Level filtering
    if (level < logger->level) {
        return;
    }

    // Build log line into CtxBuf (cross-platform, no open_memstream)
    CtxBuf b;
    ctxbuf_init(&b, 512);
    if (!b.data)
        return;

    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    // Get short filename
    const char *filename = get_filename(source_file);
    bool show_source = logger->add_source && source_line > 0;

    if (logger->format == XR_LOG_FORMAT_JSON) {
        // JSON format output
        ctxbuf_printf(&b, "{\"time\":\"%s\",\"level\":\"%s\"", timestamp, xr_log_level_name(level));

        // Source location
        if (show_source) {
            if (filename) {
                ctxbuf_printf(&b, ",\"source\":\"%s:%d\"", filename, source_line);
            } else {
                ctxbuf_printf(&b, ",\"line\":%d", source_line);
            }
        }

        ctxbuf_append(&b, ",\"msg\":", 7);
        write_json_string_buf(&b, msg);

        // Output inherited context
        if (logger->json_ctx && logger->json_ctx_len > 0) {
            ctxbuf_putc(&b, ',');
            ctxbuf_append(&b, logger->json_ctx, logger->json_ctx_len);
        }

        // Output attributes
        char vbuf[64];
        for (int i = 0; i < nattrs; i++) {
            XrValue key = attrs[i * 2];
            XrValue val = attrs[i * 2 + 1];

            const char *key_str = value_to_string(key, vbuf, sizeof(vbuf));
            ctxbuf_printf(&b, ",\"%s\":", key_str);

            if (XR_IS_STRING(val)) {
                write_json_string_buf(&b, XR_STRING_CHARS(XR_TO_STRING(val)));
            } else if (XR_IS_INT(val)) {
                ctxbuf_printf(&b, "%lld", (long long) XR_TO_INT(val));
            } else if (XR_IS_FLOAT(val)) {
                ctxbuf_printf(&b, "%.17g", XR_TO_FLOAT(val));
            } else if (XR_IS_BOOL(val)) {
                ctxbuf_append(&b, XR_TO_BOOL(val) ? "true" : "false", XR_TO_BOOL(val) ? 4 : 5);
            } else if (XR_IS_NULL(val)) {
                ctxbuf_append(&b, "null", 4);
            } else {
                write_json_string_buf(&b, "<object>");
            }
        }

        ctxbuf_append(&b, "}\n", 2);
    } else {
        // Text format output
        if (show_source) {
            if (filename) {
                ctxbuf_printf(&b, "%s %-5s [%s:%d] %s", timestamp, xr_log_level_name(level),
                              filename, source_line, msg);
            } else {
                ctxbuf_printf(&b, "%s %-5s [L%d] %s", timestamp, xr_log_level_name(level),
                              source_line, msg);
            }
        } else {
            ctxbuf_printf(&b, "%s %-5s %s", timestamp, xr_log_level_name(level), msg);
        }

        // Output inherited context
        if (logger->text_ctx && logger->text_ctx_len > 0) {
            ctxbuf_putc(&b, ' ');
            ctxbuf_append(&b, logger->text_ctx, logger->text_ctx_len);
        }

        // Output attributes
        char vbuf[64];
        for (int i = 0; i < nattrs; i++) {
            XrValue key = attrs[i * 2];
            XrValue val = attrs[i * 2 + 1];

            const char *key_str = value_to_string(key, vbuf, sizeof(vbuf));
            char val_buf[64];
            const char *val_str = value_to_string(val, val_buf, sizeof(val_buf));

            if (needs_quoting(val_str)) {
                ctxbuf_printf(&b, " %s=\"%s\"", key_str, val_str);
            } else {
                ctxbuf_printf(&b, " %s=%s", key_str, val_str);
            }
        }

        ctxbuf_putc(&b, '\n');
    }

    // Async: transfer buffer ownership to queue; Sync: write and free
    if (logger->async_mode && ls && ls->async_initialized) {
        async_log_write(ls, b.data);  // ownership transferred
    } else {
        FILE *out = logger->output ? logger->output : stderr;
        fwrite(b.data, 1, b.len, out);
        // Only flush for WARN+ level (P2-3: smart flush)
        if (level >= XR_LOG_WARN) {
            fflush(out);
        }
        xr_free(b.data);
    }
}

// Simplified version without source location
static void xr_log_write(XrLogState *ls, XrLogger *logger, XrLogLevel level, const char *msg,
                         XrValue *attrs, int nattrs) {
    xr_log_write_ex(ls, logger, level, msg, attrs, nattrs, NULL, 0);
}

/* ========== VM Binding Functions ========== */

// Extract message and attributes from args
static void extract_log_args(XrValue *args, int nargs, const char **msg, XrValue **attrs,
                             int *nattrs) {
    *msg = "";
    *attrs = NULL;
    *nattrs = 0;

    if (nargs < 1)
        return;

    // First arg is message
    if (XR_IS_STRING(args[0])) {
        *msg = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    }

    // Remaining args are key-value pairs
    if (nargs > 1) {
        *attrs = &args[1];
        *nattrs = (nargs - 1) / 2;
    }
}

// Get current source file and line number from VM frame stack
static void get_source_location(XrayIsolate *isolate, const char **out_file, int *out_line) {
    *out_file = NULL;
    *out_line = 0;
    if (!isolate || isolate->vm.frame_count == 0)
        return;

    // Walk frame stack to find first valid xray frame
    for (int i = isolate->vm.frame_count - 1; i >= 0; i--) {
        XrBcCallFrame *frame = &isolate->vm.frames[i];
        if (!frame->closure || !frame->closure->proto)
            continue;

        XrProto *proto = frame->closure->proto;

        // Get line number from lineinfo
        if (proto->lineinfo.count > 0 && frame->pc) {
            int pc_offset = (int) (frame->pc - 1 - PROTO_CODE_BASE(proto));
            if (pc_offset >= 0 && pc_offset < (int) proto->code.count) {
                int line = PROTO_LINE(proto, pc_offset);
                if (line > 0) {
                    *out_line = line;
                    *out_file = proto->source_file;
                    return;
                }
            }
        }
    }
}

// Log output with source location
static void log_with_source(XrayIsolate *isolate, XrLogLevel level, const char *msg, XrValue *attrs,
                            int nattrs) {
    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;
    const char *file = NULL;
    int line = 0;

    if (logger->add_source) {
        get_source_location(isolate, &file, &line);
    }

    xr_log_write_ex(ls, logger, level, msg, attrs, nattrs, file, line);
}

static XrValue xr_log_debug(XrayIsolate *isolate, XrValue *args, int nargs) {
    const char *msg;
    XrValue *attrs;
    int nattrs;
    extract_log_args(args, nargs, &msg, &attrs, &nattrs);
    log_with_source(isolate, XR_LOG_DEBUG, msg, attrs, nattrs);
    return xr_null();
}

static XrValue xr_log_info(XrayIsolate *isolate, XrValue *args, int nargs) {
    const char *msg;
    XrValue *attrs;
    int nattrs;
    extract_log_args(args, nargs, &msg, &attrs, &nattrs);
    log_with_source(isolate, XR_LOG_INFO, msg, attrs, nattrs);
    return xr_null();
}

static XrValue xr_log_warn(XrayIsolate *isolate, XrValue *args, int nargs) {
    const char *msg;
    XrValue *attrs;
    int nattrs;
    extract_log_args(args, nargs, &msg, &attrs, &nattrs);
    log_with_source(isolate, XR_LOG_WARN, msg, attrs, nattrs);
    return xr_null();
}

static XrValue xr_log_error(XrayIsolate *isolate, XrValue *args, int nargs) {
    const char *msg;
    XrValue *attrs;
    int nattrs;
    extract_log_args(args, nargs, &msg, &attrs, &nattrs);
    log_with_source(isolate, XR_LOG_ERROR, msg, attrs, nattrs);
    return xr_null();
}

static XrValue xr_log_fatal(XrayIsolate *isolate, XrValue *args, int nargs) {
    const char *msg;
    XrValue *attrs;
    int nattrs;
    extract_log_args(args, nargs, &msg, &attrs, &nattrs);
    log_with_source(isolate, XR_LOG_FATAL, msg, attrs, nattrs);
    // Flush async queue before exit
    XrLogState *ls = log_state_get(isolate);
    if (ls->async_initialized)
        async_queue_flush(ls);
    exit(1);
    return xr_null();  // Unreachable
}

static XrValue xr_log_enable_source(XrayIsolate *isolate, XrValue *args, int nargs) {
    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;

    xr_mutex_lock(&ls->mutex);
    if (nargs >= 1 && XR_IS_BOOL(args[0])) {
        logger->add_source = XR_TO_BOOL(args[0]);
    } else {
        // Toggle if no argument
        logger->add_source = !logger->add_source;
    }
    xr_mutex_unlock(&ls->mutex);
    return xr_null();
}

static XrValue xr_log_enable_async(XrayIsolate *isolate, XrValue *args, int nargs) {
    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;

    bool enable = true;
    if (nargs >= 1 && XR_IS_BOOL(args[0])) {
        enable = XR_TO_BOOL(args[0]);
    }

    // Guard the async transition with the logger mutex: flipping
    // async_mode while a concurrent log.info() call is reading the flag
    // would otherwise race the async_queue init/teardown sequence.
    xr_mutex_lock(&ls->mutex);
    if (enable && !logger->async_mode) {
        async_queue_init(ls, logger->output);
        logger->async_mode = true;
    } else if (!enable && logger->async_mode) {
        async_queue_flush(ls);
        async_queue_stop(ls);
        logger->async_mode = false;
    }
    xr_mutex_unlock(&ls->mutex);
    return xr_null();
}

static XrValue xr_log_flush(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;

    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;
    if (logger->async_mode) {
        async_queue_flush(ls);
    }

    return xr_null();
}

static XrValue xr_log_set_level(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1) {
        fprintf(stderr, "log.setLevel() requires 1 argument\n");
        return xr_null();
    }

    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;
    xr_mutex_lock(&ls->mutex);
    if (XR_IS_INT(args[0])) {
        logger->level = (XrLogLevel) XR_TO_INT(args[0]);
    } else if (XR_IS_STRING(args[0])) {
        logger->level = xr_log_level_parse(XR_STRING_CHARS(XR_TO_STRING(args[0])));
    }
    xr_mutex_unlock(&ls->mutex);
    return xr_null();
}

static XrValue xr_log_set_format(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) {
        fprintf(stderr, "log.setFormat() requires a string argument\n");
        return xr_null();
    }

    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;
    const char *format = XR_STRING_CHARS(XR_TO_STRING(args[0]));

    xr_mutex_lock(&ls->mutex);
    if (strcasecmp(format, "json") == 0) {
        logger->format = XR_LOG_FORMAT_JSON;
    } else if (strcasecmp(format, "text") == 0) {
        logger->format = XR_LOG_FORMAT_TEXT;
    } else {
        fprintf(stderr, "log.setFormat(): unknown format '%s', use 'json' or 'text'\n", format);
    }
    xr_mutex_unlock(&ls->mutex);
    return xr_null();
}

static XrValue xr_log_set_output(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) {
        fprintf(stderr, "log.setOutput() requires a string argument\n");
        return xr_null();
    }

    XrLogState *ls = log_state_get(isolate);
    XrLogger *logger = &ls->default_logger;
    const char *output = XR_STRING_CHARS(XR_TO_STRING(args[0]));

    // Open the new stream outside the critical section so fopen blocking
    // on disk does not stall concurrent log.info() calls.
    FILE *new_out = NULL;
    bool opened_file = false;
    if (strcasecmp(output, "stdout") == 0) {
        new_out = stdout;
    } else if (strcasecmp(output, "stderr") == 0) {
        new_out = stderr;
    } else {
        new_out = fopen(output, "a");
        if (!new_out) {
            fprintf(stderr, "log.setOutput(): failed to open file '%s'\n", output);
            new_out = stderr;
        } else {
            opened_file = true;
        }
    }

    xr_mutex_lock(&ls->mutex);
    FILE *old_out = logger->output;
    logger->output = new_out;
    xr_mutex_unlock(&ls->mutex);

    // Close the previous file handle only after the swap so any in-flight
    // log write that already sampled the old pointer can finish draining
    // its buffer on stderr-fallback cleanly.
    if (old_out != NULL && old_out != stderr && old_out != stdout) {
        fclose(old_out);
    }
    (void) opened_file;
    return xr_null();
}

static XrValue xr_log_is_enabled(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_INT(args[0])) {
        return xr_bool(false);
    }

    XrLogState *ls = log_state_get(isolate);
    XrLogLevel level = (XrLogLevel) XR_TO_INT(args[0]);

    return xr_bool(level >= ls->default_logger.level);
}

/* ========== Child Logger Implementation ========== */

static XrValue wrap_logger(XrayIsolate *X, XrLogger *logger) {
    XrCoroutine *coro = xr_current_coro(X);
    XrLoggerRef *ref = (XrLoggerRef *) xr_alloc(coro, sizeof(XrLoggerRef), XR_TLOGGER);
    if (!ref)
        return xr_null();
    ref->logger = logger;
    return XR_FROM_PTR(ref);
}

static XrLogger *unwrap_logger(XrayIsolate *X, XrValue v) {
    (void) X;
    if (!XR_IS_PTR(v))
        return NULL;
    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(v);
    if (XR_GC_GET_TYPE(gc) != XR_TLOGGER)
        return NULL;
    return ((XrLoggerRef *) gc)->logger;
}

// Create child logger
static XrLogger *create_child_logger(XrLogger *parent, XrValue *attrs, int nattrs) {
    XR_DCHECK(parent != NULL, "create_child_logger: parent must not be NULL");
    XrLogger *child = (XrLogger *) xr_malloc(sizeof(XrLogger));
    if (!child)
        return NULL;

    // Inherit parent logger configuration
    child->level = parent->level;
    child->format = parent->format;
    child->output = parent->output;
    child->add_source = parent->add_source;
    child->async_mode = parent->async_mode;
    child->parent = parent;

    // Build JSON context: "key":"val","k2":123
    CtxBuf jbuf;
    ctxbuf_init(&jbuf, 256);

    // Build Text context: key=val k2=123
    CtxBuf tbuf;
    ctxbuf_init(&tbuf, 256);

    if (!jbuf.data || !tbuf.data) {
        xr_free(jbuf.data);
        xr_free(tbuf.data);
        xr_free(child);
        return NULL;
    }

    // Inherit parent context
    if (parent->json_ctx && parent->json_ctx_len > 0) {
        ctxbuf_append(&jbuf, parent->json_ctx, parent->json_ctx_len);
    }
    if (parent->text_ctx && parent->text_ctx_len > 0) {
        ctxbuf_append(&tbuf, parent->text_ctx, parent->text_ctx_len);
    }

    // Add new context attributes
    char vbuf[64];
    for (int i = 0; i < nattrs; i++) {
        XrValue key = attrs[i * 2];
        XrValue val = attrs[i * 2 + 1];

        const char *key_str = value_to_string(key, vbuf, sizeof(vbuf));

        // === JSON context ===
        if (jbuf.len > 0)
            ctxbuf_putc(&jbuf, ',');
        ctxbuf_putc(&jbuf, '"');
        ctxbuf_append(&jbuf, key_str, (int) strlen(key_str));
        ctxbuf_append(&jbuf, "\":", 2);

        if (XR_IS_STRING(val)) {
            write_json_string_buf(&jbuf, XR_STRING_CHARS(XR_TO_STRING(val)));
        } else if (XR_IS_INT(val)) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", (long long) XR_TO_INT(val));
            ctxbuf_append(&jbuf, tmp, n);
        } else if (XR_IS_FLOAT(val)) {
            // 32 bytes is enough for the longest %.17g formatting of a
            // finite double: sign + 17 digits + 'e' + sign + 3-digit exp.
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%.17g", XR_TO_FLOAT(val));
            ctxbuf_append(&jbuf, tmp, n);
        } else if (XR_IS_BOOL(val)) {
            const char *s = XR_TO_BOOL(val) ? "true" : "false";
            ctxbuf_append(&jbuf, s, (int) strlen(s));
        } else {
            ctxbuf_append(&jbuf, "null", 4);
        }

        // === Text context ===
        if (tbuf.len > 0)
            ctxbuf_putc(&tbuf, ' ');
        ctxbuf_append(&tbuf, key_str, (int) strlen(key_str));
        ctxbuf_putc(&tbuf, '=');

        char val_buf[64];
        const char *val_str = value_to_string(val, val_buf, sizeof(val_buf));
        if (needs_quoting(val_str)) {
            ctxbuf_putc(&tbuf, '"');
            ctxbuf_append(&tbuf, val_str, (int) strlen(val_str));
            ctxbuf_putc(&tbuf, '"');
        } else {
            ctxbuf_append(&tbuf, val_str, (int) strlen(val_str));
        }
    }

    child->json_ctx = jbuf.data;
    child->json_ctx_len = jbuf.len;
    child->text_ctx = tbuf.data;
    child->text_ctx_len = tbuf.len;

    return child;
}

static XrValue xr_log_child(XrayIsolate *isolate, XrValue *args, int nargs) {
    XrLogger *parent = log_default(isolate);
    XrLogger *child = create_child_logger(parent, args, nargs / 2);
    if (!child)
        return xr_null();
    return wrap_logger(isolate, child);
}

// Common implementation for child logger methods
static XrValue logger_log_at(XrayIsolate *isolate, XrValue *args, int nargs, XrLogLevel level) {
    if (nargs < 1)
        return xr_null();
    XrLogger *logger = unwrap_logger(isolate, args[0]);
    if (!logger)
        return xr_null();

    const char *msg = "";
    if (nargs > 1 && XR_IS_STRING(args[1])) {
        msg = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    }

    XrValue *attrs = (nargs > 2) ? &args[2] : NULL;
    int nattrs = (nargs > 2) ? (nargs - 2) / 2 : 0;

    XrLogState *ls = log_state_get(isolate);
    xr_log_write(ls, logger, level, msg, attrs, nattrs);
    return xr_null();
}

static XrValue xr_logger_debug(XrayIsolate *X, XrValue *args, int n) {
    return logger_log_at(X, args, n, XR_LOG_DEBUG);
}
static XrValue xr_logger_info(XrayIsolate *X, XrValue *args, int n) {
    return logger_log_at(X, args, n, XR_LOG_INFO);
}
static XrValue xr_logger_warn(XrayIsolate *X, XrValue *args, int n) {
    return logger_log_at(X, args, n, XR_LOG_WARN);
}
static XrValue xr_logger_error(XrayIsolate *X, XrValue *args, int n) {
    return logger_log_at(X, args, n, XR_LOG_ERROR);
}

static XrValue xr_logger_fatal(XrayIsolate *X, XrValue *args, int n) {
    logger_log_at(X, args, n, XR_LOG_FATAL);
    // Flush async queue before exit
    XrLogState *ls = log_state_get(X);
    if (ls->async_initialized)
        async_queue_flush(ls);
    exit(1);
    return xr_null();
}

static XrValue xr_logger_child(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();
    XrLogger *parent = unwrap_logger(isolate, args[0]);
    if (!parent)
        return xr_null();

    XrValue *attrs = (nargs > 1) ? &args[1] : NULL;
    int nattrs = (nargs > 1) ? (nargs - 1) / 2 : 0;

    XrLogger *child = create_child_logger(parent, attrs, nattrs);
    if (!child)
        return xr_null();
    return wrap_logger(isolate, child);
}

/* ========== Log State Cleanup ========== */

// Destroy the per-isolate log state. Called from xr_stdlib_cache_free via
// the log_state_cleanup function pointer.
static void log_state_destroy(void *opaque) {
    XrLogState *ls = (XrLogState *) opaque;
    if (!ls)
        return;

    // Tear down the async writer thread if it is still running.
    if (ls->async_initialized) {
        async_queue_stop(ls);
    }

    // Close a custom output file (never close stderr/stdout).
    FILE *out = ls->default_logger.output;
    if (out && out != stderr && out != stdout) {
        fclose(out);
    }

    xr_mutex_destroy(&ls->mutex);
    xr_free(ls);
}

/* ========== GC Destroy ========== */

// Declared (with XR_FUNC) in src/runtime/gc/xgc_internal.h.
// Called by GC when XrLoggerRef is collected.
void xr_gc_destroy_logger(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    XrLoggerRef *ref = (XrLoggerRef *) obj;
    if (ref->logger) {
        xr_free(ref->logger->json_ctx);
        xr_free(ref->logger->text_ctx);
        xr_free(ref->logger);
        ref->logger = NULL;
    }
}

/* ========== Module Loader ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module log

XR_DEFINE_BUILTIN(xr_log_debug, "debug", "(...args: any): void", "Log debug message")
XR_DEFINE_BUILTIN(xr_log_info, "info", "(...args: any): void", "Log info message")
XR_DEFINE_BUILTIN(xr_log_warn, "warn", "(...args: any): void", "Log warning message")
XR_DEFINE_BUILTIN(xr_log_error, "error", "(...args: any): void", "Log error message")
XR_DEFINE_BUILTIN(xr_log_fatal, "fatal", "(...args: any): void", "Log fatal message")
XR_DEFINE_BUILTIN(xr_log_set_level, "setLevel", "(level: int): void", "Set log level")
XR_DEFINE_BUILTIN(xr_log_set_format, "setFormat", "(format: string): void", "Set log format")
XR_DEFINE_BUILTIN(xr_log_set_output, "setOutput", "(path: string): void", "Set log output file")
XR_DEFINE_BUILTIN(xr_log_is_enabled, "isEnabled", "(level: int): bool",
                  "Check if log level enabled")
XR_DEFINE_BUILTIN(xr_log_enable_source, "enableSource", "(enabled: bool): void",
                  "Enable source location in logs")
XR_DEFINE_BUILTIN(xr_log_enable_async, "enableAsync", "(enabled: bool): void",
                  "Enable async logging")
XR_DEFINE_BUILTIN(xr_log_flush, "flush", "(): void", "Flush log buffer")
XR_DEFINE_BUILTIN(xr_log_child, "child", "(...fields: any): any", "Create child logger")

XrModule *xr_load_module_log(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_log: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "log");
    if (!module)
        return NULL;

    // Log functions
    XRS_EXPORT(module, isolate, "debug", xr_log_debug);
    XRS_EXPORT(module, isolate, "info", xr_log_info);
    XRS_EXPORT(module, isolate, "warn", xr_log_warn);
    XRS_EXPORT(module, isolate, "error", xr_log_error);
    XRS_EXPORT(module, isolate, "fatal", xr_log_fatal);

    // Config functions
    XRS_EXPORT(module, isolate, "setLevel", xr_log_set_level);
    XRS_EXPORT(module, isolate, "setFormat", xr_log_set_format);
    XRS_EXPORT(module, isolate, "setOutput", xr_log_set_output);
    XRS_EXPORT(module, isolate, "isEnabled", xr_log_is_enabled);
    XRS_EXPORT(module, isolate, "enableSource", xr_log_enable_source);
    XRS_EXPORT(module, isolate, "enableAsync", xr_log_enable_async);
    XRS_EXPORT(module, isolate, "flush", xr_log_flush);

    // Child logger
    XRS_EXPORT(module, isolate, "child", xr_log_child);

    // Export constants
    xr_module_add_export(isolate, module, "DEBUG", xr_int(XR_LOG_DEBUG));
    xr_module_add_export(isolate, module, "INFO", xr_int(XR_LOG_INFO));
    xr_module_add_export(isolate, module, "WARN", xr_int(XR_LOG_WARN));
    xr_module_add_export(isolate, module, "ERROR", xr_int(XR_LOG_ERROR));
    xr_module_add_export(isolate, module, "FATAL", xr_int(XR_LOG_FATAL));

    module->loaded = true;

    // Register Logger as native type for method dispatch via native_type_classes
    static XrNativeMethod logger_methods[] = {{"debug", xr_logger_debug, -1},
                                              {"info", xr_logger_info, -1},
                                              {"warn", xr_logger_warn, -1},
                                              {"error", xr_logger_error, -1},
                                              {"fatal", xr_logger_fatal, -1},
                                              {"child", xr_logger_child, -1},
                                              {NULL, NULL, 0}};

    XrNativeTypeInfo logger_type_info = {.name = "Logger",
                                         .gc_type = XR_TLOGGER,
                                         .methods = logger_methods,
                                         .getters = NULL,
                                         .static_methods = NULL};

    xr_register_native_type(isolate, &logger_type_info);

    return module;
}
