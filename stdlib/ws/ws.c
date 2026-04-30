/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws.c - WebSocket protocol implementation
 *
 * KEY CONCEPT:
 *   Implements RFC 6455 WebSocket protocol
 */

#include "ws.h"
#include "ws_deflate.h"
#include "../../src/os/os_net.h"
#include "../../src/os/os_random.h"
#include "../../src/base/xmalloc.h"
#include "../../src/os/os_time.h"
#include "../../src/coro/xsocket.h"
#include "../../src/runtime/object/xutf8.h"
#include "../base64/base64.h"
#include "../net/io.h"
#include "../../src/io/xdns.h"
#include "../net/tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef XR_OS_MACOS
#include <CommonCrypto/CommonDigest.h>
#define SHA1(data, len, hash) CC_SHA1(data, (CC_LONG) (len), hash)
#elif defined(XR_OS_WINDOWS)
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
// Minimal SHA1 shim using Windows CNG (BCrypt)
static inline unsigned char *SHA1(const unsigned char *data, size_t len, unsigned char *hash) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0);
    BCRYPT_HASH_HANDLE h = NULL;
    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    BCryptHashData(h, (PUCHAR) data, (ULONG) len, 0);
    BCryptFinishHash(h, hash, 20, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash;
}
#else
#include <openssl/sha.h>
#endif

static uint64_t ws_now_ms(void) {
    return xr_time_monotonic_ms();
}

// WebSocket frame header constants
#define WS_MAX_HEADER_SIZE 14  // Max frame header: 2 + 8 (extended len) + 4 (mask)
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// Default max message size (can be configured per-connection)
#define WS_DEFAULT_MAX_MESSAGE_SIZE (16 * 1024 * 1024)  // 16MB

/* ========== Header parsing helpers (shared by client + server) ========== */

static const char *ws_find_header(const char *headers, const char *name);
static bool ws_get_header_value(const char *headers, const char *name, char *out, size_t out_size);
static bool ws_header_list_has_token(const char *value, const char *token);

/* ========== Thread-local storage ========== */

#ifndef XR_THREAD_LOCAL
#ifdef __GNUC__
#define XR_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#define XR_THREAD_LOCAL __declspec(thread)
#else
#define XR_THREAD_LOCAL
#endif
#endif

/* ========== Frame Buffer Pool (thread-local, zero contention) ========== */

#define WS_STACK_THRESHOLD 512  // Stack allocation threshold
#define WS_POOL_CLASSES 4
#define WS_POOL_MAX_CACHED 32  // Max cached buffers per size class

// Size classes: 256B, 4KB, 64KB, 1MB
static const size_t ws_pool_sizes[WS_POOL_CLASSES] = {256, 4096, 65536, 1048576};

typedef struct {
    char *head;  // Intrusive freelist (next pointer stored in buffer)
    int count;
} WsPoolClass;

typedef struct {
    WsPoolClass classes[WS_POOL_CLASSES];
} WsBufferPool;

static XR_THREAD_LOCAL WsBufferPool ws_pool;

static inline int ws_pool_class(size_t size) {
    for (int i = 0; i < WS_POOL_CLASSES; i++) {
        if (size <= ws_pool_sizes[i])
            return i;
    }
    return -1;  // Too large for pool
}

static inline char *ws_buffer_alloc(size_t size) {
    int cls = ws_pool_class(size);
    if (cls >= 0 && ws_pool.classes[cls].head) {
        char *buf = ws_pool.classes[cls].head;
        ws_pool.classes[cls].head = *(char **) buf;
        ws_pool.classes[cls].count--;
        return buf;
    }
    size_t alloc_size = (cls >= 0) ? ws_pool_sizes[cls] : size;
    return (char *) xr_malloc(alloc_size);
}

static inline void ws_buffer_free(char *buf, size_t size) {
    int cls = ws_pool_class(size);
    if (cls >= 0 && ws_pool.classes[cls].count < WS_POOL_MAX_CACHED) {
        *(char **) buf = ws_pool.classes[cls].head;
        ws_pool.classes[cls].head = buf;
        ws_pool.classes[cls].count++;
        return;
    }
    xr_free(buf);
}

/* ========== Per-connection flat read buffer ========== */

static inline bool ws_rbuf_ensure(XrWebSocket *ws) {
    if (ws->rbuf)
        return true;
    ws->rbuf = (char *) xr_malloc(WS_RBUF_INIT_CAP);
    if (!ws->rbuf)
        return false;
    ws->rbuf_off = 0;
    ws->rbuf_len = 0;
    ws->rbuf_cap = WS_RBUF_INIT_CAP;
    return true;
}

static inline void ws_rbuf_consume(XrWebSocket *ws, int n) {
    ws->rbuf_off += n;
    ws->rbuf_len -= n;
    if (ws->rbuf_len == 0)
        ws->rbuf_off = 0;  // fast reset
}

static inline void ws_rbuf_compact(XrWebSocket *ws) {
    if (ws->rbuf_off > 0 && ws->rbuf_len > 0) {
        memmove(ws->rbuf, ws->rbuf + ws->rbuf_off, ws->rbuf_len);
    }
    ws->rbuf_off = 0;
}

// Generate Sec-WebSocket-Key (using secure random)
static char *generate_sec_key(void) {
    unsigned char key[16];
    xr_random_bytes(key, 16);
    return xr_base64_encode(key, 16, NULL);
}

// Calculate Sec-WebSocket-Accept
static char *compute_accept_key(const char *sec_key) {
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", sec_key, WS_GUID);

    unsigned char hash[20];
    SHA1((unsigned char *) concat, strlen(concat), hash);

    return xr_base64_encode(hash, 20, NULL);
}

/*
 * Parse WebSocket URL. Supports:
 *   ws://host/path           — plain, default port 80
 *   wss://host/path          — TLS, default port 443
 *   ws://host:port/path      — explicit port
 *   ws://[::1]:port/path     — IPv6 literal (RFC 3986 bracket notation)
 *   ws://[::1]/path          — IPv6 literal, default port
 */
static int parse_ws_url(const char *url, char **host, int *port, char **path, bool *secure) {
    *host = NULL;
    *path = NULL;
    *port = 80;
    *secure = false;

    const char *p = url;

    if (strncmp(p, "wss://", 6) == 0) {
        *secure = true;
        *port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        return -1;
    }

    // Extract host — handle IPv6 bracket notation
    if (*p == '[') {
        // IPv6 literal: ws://[::1]:8080/path
        p++;  // skip '['
        const char *host_start = p;
        while (*p && *p != ']')
            p++;
        if (*p != ']')
            return -1;  // unterminated bracket
        *host = xr_strndup(host_start, (size_t) (p - host_start));
        p++;  // skip ']'
    } else {
        const char *host_start = p;
        while (*p && *p != ':' && *p != '/' && *p != '?')
            p++;
        *host = xr_strndup(host_start, (size_t) (p - host_start));
    }
    if (!*host)
        return -1;

    // Extract port
    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && *p != '/' && *p != '?')
            p++;
    }

    // Extract path
    if (*p == '/' || *p == '?') {
        *path = xr_strdup(p);
    } else {
        *path = xr_strdup("/");
    }

    return 0;
}

/*
 * Raw recv (TLS-aware).
 *
 * All reads route through coroutine-aware I/O: plain-TCP via xr_socket_read
 * (yields on EAGAIN via netpoll), TLS via xr_tls_conn_read (yields when
 * the supplied isolate has a runtime to suspend the calling coroutine).
 *
 * Requires ws->isolate to be bound (enforced by xr_ws_connect and
 * xr_ws_upgrade). There is no legacy fallback.
 */
static ssize_t ws_raw_recv(XrWebSocket *ws, void *buf, size_t len) {
    XR_DCHECK(ws->isolate != NULL, "ws: isolate required for I/O");
    if (ws->is_secure && ws->tls_conn) {
        return xr_tls_conn_read(ws->isolate, (XrTlsConn *) ws->tls_conn, buf, len);
    }
    int n = xr_socket_read(ws->isolate, ws->fd, (char *) buf, len);
    return (ssize_t) n;
}

/*
 * recv with timeout.
 *
 * Used by the client handshake where a bounded wait is needed.
 * timeout_ms is informational — cancellation comes from scheduler
 * deadlines. The actual read yields via netpoll.
 */
static ssize_t ws_recv_timeout(XrWebSocket *ws, void *buf, size_t len, int timeout_ms) {
    (void) timeout_ms;  // enforced by scheduler deadlines, not poll()
    return ws_raw_recv(ws, buf, len);
}

/*
 * Send all data (TLS-aware).
 *
 * Plain-TCP sends route through xr_socket_write which auto-yields on
 * EAGAIN via netpoll. TLS via xr_tls_conn_write (yields when ws->isolate
 * is bound to a runtime). Requires ws->isolate to be bound.
 */
static int ws_send_all(XrWebSocket *ws, const void *buf, size_t len) {
    XR_DCHECK(ws->isolate != NULL, "ws: isolate required for send");
    const char *p = (const char *) buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n;
        if (ws->is_secure && ws->tls_conn) {
            n = xr_tls_conn_write(ws->isolate, (XrTlsConn *) ws->tls_conn, p + sent, len - sent);
        } else {
            n = xr_socket_write(ws->isolate, ws->fd, p + sent, len - sent);
        }
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

/* ========== Optimized Frame Send with writev ========== */

// Build frame header into provided buffer (max 14 bytes)
// Returns header length, fills mask_key if mask=true
// rsv1: set RSV1 bit (0x40) for permessage-deflate compressed frames
static size_t build_frame_header(char *header, XrWsOpcode opcode, size_t payload_len, bool mask,
                                 unsigned char *mask_key, bool rsv1) {
    char *p = header;

    // First byte: FIN + RSV1 + opcode
    *p++ = (char) (0x80 | (rsv1 ? 0x40 : 0x00) | (opcode & 0x0F));

    // Second byte: MASK + payload length
    if (payload_len > 65535) {
        *p++ = (char) (mask ? 0xFF : 0x7F);
        // 8-byte length (network byte order)
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = (char) ((payload_len >> 24) & 0xFF);
        *p++ = (char) ((payload_len >> 16) & 0xFF);
        *p++ = (char) ((payload_len >> 8) & 0xFF);
        *p++ = (char) (payload_len & 0xFF);
    } else if (payload_len > 125) {
        *p++ = (char) (mask ? 0xFE : 0x7E);
        *p++ = (char) ((payload_len >> 8) & 0xFF);
        *p++ = (char) (payload_len & 0xFF);
    } else {
        *p++ = (char) ((mask ? 0x80 : 0x00) | (payload_len & 0x7F));
    }

    // Masking key
    if (mask) {
        xr_random_bytes(mask_key, 4);
        memcpy(p, mask_key, 4);
        p += 4;
    }

    return (size_t) (p - header);
}

// Apply XOR mask to data (in-place for send buffer, or to dest for zero-copy)
static void apply_mask(const unsigned char *src, unsigned char *dst, size_t len,
                       const unsigned char *mask_key) {
    // Optimize: process 8 bytes at a time when possible
    size_t i = 0;

    // Process 8 bytes at a time (unroll for performance)
    if (len >= 8) {
        uint64_t mask64 = ((uint64_t) mask_key[0]) | ((uint64_t) mask_key[1] << 8) |
                          ((uint64_t) mask_key[2] << 16) | ((uint64_t) mask_key[3] << 24) |
                          ((uint64_t) mask_key[0] << 32) | ((uint64_t) mask_key[1] << 40) |
                          ((uint64_t) mask_key[2] << 48) | ((uint64_t) mask_key[3] << 56);

        for (; i + 8 <= len; i += 8) {
            uint64_t *dst64 = (uint64_t *) (dst + i);
            const uint64_t *src64 = (const uint64_t *) (src + i);
            *dst64 = *src64 ^ mask64;
        }
    }

    // Process remaining bytes
    for (; i < len; i++) {
        dst[i] = src[i] ^ mask_key[i % 4];
    }
}

// In-place unmask with 32-byte unrolled XOR (server recv hot path)
static void unmask_inplace(unsigned char *data, size_t len, const unsigned char *mask_key) {
    size_t i = 0;
    uint64_t mask64 = ((uint64_t) mask_key[0]) | ((uint64_t) mask_key[1] << 8) |
                      ((uint64_t) mask_key[2] << 16) | ((uint64_t) mask_key[3] << 24) |
                      ((uint64_t) mask_key[0] << 32) | ((uint64_t) mask_key[1] << 40) |
                      ((uint64_t) mask_key[2] << 48) | ((uint64_t) mask_key[3] << 56);
    // 32-byte unrolled loop (helps compiler auto-vectorize)
    for (; i + 32 <= len; i += 32) {
        uint64_t *p = (uint64_t *) (data + i);
        p[0] ^= mask64;
        p[1] ^= mask64;
        p[2] ^= mask64;
        p[3] ^= mask64;
    }
    for (; i + 8 <= len; i += 8) {
        *(uint64_t *) (data + i) ^= mask64;
    }
    for (; i < len; i++) {
        data[i] ^= mask_key[i & 3];
    }
}

// Send frame using writev for zero-copy (server side, no masking)
static int ws_send_frame_writev(XrWebSocket *ws, XrWsOpcode opcode, const void *data, size_t len) {
    // Build header on stack
    char header[WS_MAX_HEADER_SIZE];
    unsigned char mask_key[4] = {0};
    bool mask = !ws->is_server;  // Client must mask

    size_t header_len = build_frame_header(header, opcode, len, mask, mask_key, false);

    if (!mask) {
        // Server: zero-copy send with writev
        struct iovec iov[2];
        iov[0].iov_base = header;
        iov[0].iov_len = header_len;
        iov[1].iov_base = (void *) data;
        iov[1].iov_len = len;

        size_t total = header_len + len;
        size_t sent = 0;

        while (sent < total) {
            ssize_t n;
            if (ws->is_secure && ws->tls_conn) {
                // TLS doesn't support writev directly, fall back to buffered send
                if (sent < header_len) {
                    n = xr_tls_conn_write(ws->isolate, (XrTlsConn *) ws->tls_conn, header + sent,
                                          header_len - sent);
                    if (n > 0) {
                        sent += n;
                        continue;
                    }
                } else {
                    size_t data_sent = sent - header_len;
                    n = xr_tls_conn_write(ws->isolate, (XrTlsConn *) ws->tls_conn,
                                          (char *) data + data_sent, len - data_sent);
                    if (n > 0) {
                        sent += n;
                        continue;
                    }
                }
            } else {
                // Adjust iov for partial send
                if (sent < header_len) {
                    iov[0].iov_base = header + sent;
                    iov[0].iov_len = header_len - sent;
                } else {
                    iov[0].iov_len = 0;
                    size_t data_sent = sent - header_len;
                    iov[1].iov_base = (char *) data + data_sent;
                    iov[1].iov_len = len - data_sent;
                }
                n = writev(ws->fd, iov, iov[0].iov_len > 0 ? 2 : 1);
            }

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Yield until writable via netpoll
                    int wr = xr_socket_wait_writable(ws->isolate, ws->fd, 5000);
                    if (wr > 0)
                        continue;
                }
                return -1;
            }
            if (n == 0)
                return -1;
            sent += n;
        }
        return 0;
    }

    // Client: need to mask data, use pooled buffer
    size_t frame_len = header_len + len;
    char *frame = NULL;
    char stack_buf[WS_STACK_THRESHOLD];
    bool use_stack = (frame_len <= sizeof(stack_buf));

    if (use_stack) {
        frame = stack_buf;
    } else {
        frame = ws_buffer_alloc(frame_len);
        if (!frame)
            return -1;
    }

    // Copy header
    memcpy(frame, header, header_len);

    // Apply mask to payload
    apply_mask((const unsigned char *) data, (unsigned char *) (frame + header_len), len, mask_key);

    // Send
    int ret = ws_send_all(ws, frame, frame_len);

    // Free buffer
    if (!use_stack) {
        ws_buffer_free(frame, frame_len);
    }

    return ret;
}

// Non-blocking send attempt result
typedef struct {
    int result;   // 0 = success, -1 = error, -2 = would block
    size_t sent;  // bytes sent so far (for partial send)
} WsSendTryResult;

// Non-blocking send frame - for yieldable integration
// Returns: 0 = complete, -1 = error, -2 = would block (need to wait for write)
int xr_ws_send_frame_try(XrWebSocket *ws, XrWsOpcode opcode, const void *data, size_t len) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return -1;

    // Build header on stack
    char header[WS_MAX_HEADER_SIZE];
    unsigned char mask_key[4] = {0};
    bool mask = !ws->is_server;

    size_t header_len = build_frame_header(header, opcode, len, mask, mask_key, false);

    // Cork mode: append frame to wbuf, no syscall
    if (ws->corked && !mask) {
        size_t frame_len = header_len + len;
        int need = ws->wbuf_len + (int) frame_len;
        if (need > ws->wbuf_cap) {
            int newcap = ws->wbuf_cap ? ws->wbuf_cap : 4096;
            while (newcap < need)
                newcap *= 2;
            char *nb = (char *) xr_realloc(ws->wbuf, newcap);
            if (!nb)
                return -1;
            ws->wbuf = nb;
            ws->wbuf_cap = newcap;
        }
        memcpy(ws->wbuf + ws->wbuf_len, header, header_len);
        if (len > 0)
            memcpy(ws->wbuf + ws->wbuf_len + header_len, data, len);
        ws->wbuf_len += (int) frame_len;
        return 0;
    }

    if (!mask) {
        // Server: send header + payload
        // TLS path: always blocking (TLS has its own buffering)
        if (ws->is_secure && ws->tls_conn) {
            if (ws_send_all(ws, header, header_len) < 0)
                return -1;
            if (len > 0) {
                if (ws_send_all(ws, data, len) < 0)
                    return -1;
            }
            return 0;
        }

        // Resume partial send from previous non-blocking attempt
        if (ws->send_header_sent) {
            const char *p = (const char *) data + ws->send_offset;
            size_t remain = len - ws->send_offset;
            ssize_t n = send(ws->fd, p, remain, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return -2;
                ws->send_header_sent = false;
                return -1;
            }
            ws->send_offset += (size_t) n;
            if (ws->send_offset >= len) {
                ws->send_header_sent = false;
                return 0;
            }
            return -2;
        }

        size_t total = header_len + len;

        // Fast path: small frame → stack memcpy + single send (no iov overhead)
        if (total <= WS_STACK_THRESHOLD) {
            char sbuf[WS_STACK_THRESHOLD];
            memcpy(sbuf, header, header_len);
            if (len > 0)
                memcpy(sbuf + header_len, data, len);
            ssize_t n = send(ws->fd, sbuf, total, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    n = 0;
                else
                    return -1;
            }
            if ((size_t) n >= total)
                return 0;
            // Partial: header always fits in first send attempt
            size_t data_sent = ((size_t) n > header_len) ? (size_t) n - header_len : 0;
            if ((size_t) n < header_len) {
                if (ws_send_all(ws, sbuf + n, header_len - n) < 0)
                    return -1;
            }
            if (data_sent >= len)
                return 0;
            ws->send_header_sent = true;
            ws->send_offset = data_sent;
            return -2;
        }

        // Large frame: writev header + payload
        struct iovec iov[2];
        iov[0].iov_base = header;
        iov[0].iov_len = header_len;
        iov[1].iov_base = (void *) data;
        iov[1].iov_len = len;

        ssize_t n = writev(ws->fd, iov, 2);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                n = 0;
            else
                return -1;
        }
        if ((size_t) n >= total)
            return 0;

        // Partial send: finish header (tiny), then handle payload
        size_t sent = (size_t) n;
        if (sent < header_len) {
            if (ws_send_all(ws, header + sent, header_len - sent) < 0)
                return -1;
            sent = header_len;
        }

        size_t data_sent = sent - header_len;
        if (data_sent >= len)
            return 0;
        ws->send_header_sent = true;
        ws->send_offset = data_sent;
        return -2;
    }

    // Client: need to mask - use pooled buffer
    size_t frame_len = header_len + len;
    char *frame = NULL;
    char stack_buf[WS_STACK_THRESHOLD];
    bool use_stack = (frame_len <= sizeof(stack_buf));

    if (use_stack) {
        frame = stack_buf;
    } else {
        frame = ws_buffer_alloc(frame_len);
        if (!frame)
            return -1;
    }

    // Build frame
    memcpy(frame, header, header_len);
    if (data && len > 0) {
        apply_mask((const unsigned char *) data, (unsigned char *) (frame + header_len), len,
                   mask_key);
    }

    // Try non-blocking send
    ssize_t n;
    if (ws->is_secure && ws->tls_conn) {
        n = xr_tls_conn_write(ws->isolate, (XrTlsConn *) ws->tls_conn, frame, frame_len);
    } else {
        n = send(ws->fd, frame, frame_len, 0);
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Complete send with blocking helper (masked buffer can't be rebuilt)
            if (ws_send_all(ws, frame, frame_len) < 0) {
                if (!use_stack)
                    ws_buffer_free(frame, frame_len);
                return -1;
            }
            if (!use_stack)
                ws_buffer_free(frame, frame_len);
            return 0;
        }
        if (!use_stack)
            ws_buffer_free(frame, frame_len);
        return -1;
    }

    if ((size_t) n < frame_len) {
        // Partial send: complete remaining with blocking helper
        // Can't return -2 because masked buffer would be freed and rebuilt
        // with different mask keys on retry, corrupting the data stream
        if (ws_send_all(ws, frame + n, frame_len - (size_t) n) < 0) {
            if (!use_stack)
                ws_buffer_free(frame, frame_len);
            return -1;
        }
        if (!use_stack)
            ws_buffer_free(frame, frame_len);
        return 0;
    }

    if (!use_stack)
        ws_buffer_free(frame, frame_len);
    return 0;
}

/* ========== Fragment Buffer Management ========== */

// Minimum fragment buffer size (4KB)
#define WS_FRAG_MIN_SIZE 4096

// Grow factor for fragment buffer (1.5x)
#define WS_FRAG_GROW_FACTOR 3 / 2

// Ensure fragment buffer has enough capacity
// Uses exponential growth to minimize realloc calls
static int ws_frag_ensure_capacity(XrWebSocket *ws, size_t needed) {
    // Always need at least 1 byte for null terminator
    if (needed == 0)
        needed = 1;
    if (ws->frag_buf && ws->frag_buf_size >= needed)
        return 0;

    // Calculate new size with exponential growth
    size_t new_size = ws->frag_buf_size;
    if (new_size < WS_FRAG_MIN_SIZE)
        new_size = WS_FRAG_MIN_SIZE;

    while (new_size < needed) {
        new_size = new_size * WS_FRAG_GROW_FACTOR;
    }

    // Realloc with new capacity
    char *new_buf = (char *) xr_realloc(ws->frag_buf, new_size + 1);
    if (!new_buf)
        return -1;

    ws->frag_buf = new_buf;
    ws->frag_buf_size = new_size;
    return 0;
}

// Append data to fragment buffer (optimized)
static int ws_frag_append(XrWebSocket *ws, const char *data, size_t len) {
    size_t needed = ws->frag_buf_len + len;

    if (ws_frag_ensure_capacity(ws, needed) < 0)
        return -1;

    memcpy(ws->frag_buf + ws->frag_buf_len, data, len);
    ws->frag_buf_len = needed;
    ws->frag_buf[needed] = '\0';
    return 0;
}

// Start new fragment (optimized)
static int ws_frag_start(XrWebSocket *ws, XrWsOpcode opcode, const char *data, size_t len) {
    // Reset length but keep buffer if large enough
    ws->frag_buf_len = 0;
    ws->frag_opcode = opcode;

    if (ws_frag_ensure_capacity(ws, len) < 0) {
        // Fallback: allocate new buffer
        if (ws->frag_buf)
            xr_free(ws->frag_buf);
        ws->frag_buf = (char *) xr_malloc(len + 1);
        if (!ws->frag_buf) {
            ws->frag_buf_size = 0;
            return -1;
        }
        ws->frag_buf_size = len;
    }

    memcpy(ws->frag_buf, data, len);
    ws->frag_buf_len = len;
    ws->frag_buf[len] = '\0';
    return 0;
}

// Complete fragment and transfer ownership to message
static void ws_frag_complete(XrWebSocket *ws, XrWsMessage *msg) {
    msg->opcode = ws->frag_opcode;
    msg->data = ws->frag_buf;
    msg->len = ws->frag_buf_len;
    msg->is_text = (ws->frag_opcode == WS_OPCODE_TEXT);

    // Transfer ownership, reset fragment state
    ws->frag_buf = NULL;
    ws->frag_buf_size = 0;
    ws->frag_buf_len = 0;
}

/* ========== WebSocket Frame Operations ========== */

/*
 * Send a close frame using stack buffer (zero malloc).
 * Close frame payload is always ≤125 bytes per RFC 6455 Section 5.5.
 */
static int send_close_frame(XrWebSocket *ws, const void *data, size_t len, bool mask) {
    if (len > 125)
        len = 125;
    // Max frame: 2 (header) + 4 (mask) + 125 (payload) = 131
    char buf[131];
    char *p = buf;

    *p++ = (char) (0x80 | WS_OPCODE_CLOSE);
    *p++ = (char) ((mask ? 0x80 : 0x00) | (len & 0x7F));

    unsigned char mask_key[4] = {0};
    if (mask) {
        xr_random_bytes(mask_key, 4);
        memcpy(p, mask_key, 4);
        p += 4;
    }

    if (data && len > 0) {
        if (mask) {
            const unsigned char *src = (const unsigned char *) data;
            unsigned char *dst = (unsigned char *) p;
            for (size_t i = 0; i < len; i++) {
                dst[i] = src[i] ^ mask_key[i % 4];
            }
        } else {
            memcpy(p, data, len);
        }
        p += len;
    }

    return ws_send_all(ws, buf, (size_t) (p - buf));
}

// Parse WebSocket frame header
// rsv1 output: true if RSV1 bit set (permessage-deflate compressed frame)
static int parse_frame_header(const char *buf, size_t buf_len, bool *fin, XrWsOpcode *opcode,
                              bool *masked, uint64_t *payload_len, unsigned char *mask_key,
                              size_t *header_len, bool *rsv1) {
    if (buf_len < 2)
        return 0;  // Need more data

    const unsigned char *p = (const unsigned char *) buf;

    // RSV1 is allowed for permessage-deflate; RSV2/RSV3 are protocol errors
    *rsv1 = (p[0] & 0x40) != 0;
    if (p[0] & 0x30) {
        return -1;  // Protocol error: RSV2 or RSV3 set
    }

    *fin = (p[0] & 0x80) != 0;
    *opcode = (XrWsOpcode) (p[0] & 0x0F);
    *masked = (p[1] & 0x80) != 0;

    // RFC 6455 Section 5.2: reserved opcodes (0x3-0x7, 0xB-0xF) MUST fail
    uint8_t op = (uint8_t) *opcode;
    if ((op >= 0x3 && op <= 0x7) || (op >= 0xB && op <= 0xF)) {
        return -1;  // Protocol error: reserved opcode
    }

    // RFC 6455 Section 5.5: control frames MUST NOT be fragmented (FIN=1)
    bool is_control = (op & 0x8) != 0;
    if (is_control && !*fin) {
        return -1;  // Protocol error: fragmented control frame
    }

    uint64_t len = p[1] & 0x7F;
    size_t hdr_len = 2;

    // RFC 6455 Section 5.5: control frame payload MUST be 125 bytes or less
    if (is_control && len > 125) {
        return -1;  // Protocol error: control frame payload too large
    }

    if (len == 126) {
        if (buf_len < 4)
            return 0;
        len = ((uint64_t) p[2] << 8) | p[3];
        hdr_len = 4;
    } else if (len == 127) {
        if (buf_len < 10)
            return 0;
        // RFC 6455 Section 5.2: most significant bit MUST be 0
        if (p[2] & 0x80) {
            return -1;  // Protocol error: MSB of 64-bit length is set
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | p[2 + i];
        }
        hdr_len = 10;
    }

    if (*masked) {
        if (buf_len < hdr_len + 4)
            return 0;
        memcpy(mask_key, buf + hdr_len, 4);
        hdr_len += 4;
    }

    *payload_len = len;
    *header_len = hdr_len;
    return 1;
}

/* ========== API Implementation ========== */

void xr_ws_config_init(XrWsConfig *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(XrWsConfig));
    config->connect_timeout_ms = 10000;
    config->ping_interval_ms = 30000;
    config->pong_timeout_ms = 10000;
    config->max_message_size = 16 * 1024 * 1024;  // 16MB
}

XrWebSocket *xr_ws_new(const XrWsConfig *config) {
    if (!config || !config->url)
        return NULL;

    XrWebSocket *ws = (XrWebSocket *) xr_calloc(1, sizeof(XrWebSocket));
    if (!ws)
        return NULL;

    ws->state = WS_STATE_CLOSED;
    ws->fd = -1;

    // Parse URL
    if (parse_ws_url(config->url, &ws->host, &ws->port, &ws->path, &ws->is_secure) < 0) {
        xr_free(ws);
        return NULL;
    }

    // Copy config
    ws->config = *config;
    ws->config.url = xr_strdup(config->url);

    // Initialize buffer state (rbuf lazy-allocated on first recv)
    ws->rbuf = NULL;
    ws->rbuf_off = 0;
    ws->rbuf_len = 0;
    ws->rbuf_cap = 0;
    ws->msg_buf = NULL;
    ws->msg_buf_size = 0;
    ws->msg_buf_len = 0;
    ws->msg_remaining = 0;
    ws->frame_in_progress = false;
    ws->last_ping_sent_ms = 0;
    ws->last_pong_recv_ms = ws_now_ms();
    ws->ping_in_flight = false;
    ws->cached_pd = NULL;
    ws->wbuf = NULL;
    ws->wbuf_len = 0;
    ws->wbuf_cap = 0;
    ws->corked = false;

    return ws;
}

void xr_ws_set_isolate(XrWebSocket *ws, struct XrayIsolate *X) {
    // Bind the WS connection to the caller's coroutine scheduler so
    // every plain-TCP / TLS I/O path that needs to suspend can resolve
    // the runtime via ws->isolate and yield.
    if (!ws)
        return;
    ws->isolate = X;
}

void xr_ws_free(XrWebSocket *ws) {
    if (!ws)
        return;

    if (ws->state == WS_STATE_OPEN) {
        xr_ws_close(ws, WS_CLOSE_GOING_AWAY, NULL);
    }

    // Clean up TLS resources (may already be freed by xr_ws_close)
    if (ws->tls_conn) {
        xr_tls_conn_close((XrTlsConn *) ws->tls_conn);
        xr_tls_conn_free((XrTlsConn *) ws->tls_conn);
        ws->tls_conn = NULL;
    }
    if (ws->tls_ctx) {
        xr_tls_context_free((XrTlsContext *) ws->tls_ctx);
        ws->tls_ctx = NULL;
    }

    if (ws->fd >= 0)
        xr_closesocket(ws->fd);

    xr_free(ws->rbuf);
    ws->rbuf = NULL;
    xr_free(ws->wbuf);
    ws->wbuf = NULL;

    xr_free(ws->host);
    xr_free(ws->path);
    xr_free(ws->protocol);
    xr_free(ws->sec_key);
    xr_free(ws->msg_buf);
    xr_free(ws->frag_buf);
    xr_free(ws->close_reason);
    xr_free((void *) ws->config.url);
    xr_free(ws);
}

XrWsError xr_ws_connect(XrWebSocket *ws) {
    if (!ws)
        return WS_ERR_URL;
    if (ws->state == WS_STATE_OPEN)
        return WS_OK;

    XrWsError err = WS_OK;
    char *accept_key = NULL;
    char *request = NULL;
    char *response = NULL;
    ws->state = WS_STATE_CONNECTING;

    // DNS resolution (with cache, IPv4/IPv6 dual-stack).
    // NOTE: xr_dns_resolve is still synchronous — it calls getaddrinfo
    // which blocks the worker thread until the resolver returns.
    // Async DNS would need a resolver pool or c-ares integration and
    // is tracked as a separate item; WS and cluster share the same
    // getaddrinfo dependency today so improving here would be a net
    // improvement only when the full resolver path is revamped.
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(ws->isolate, ws->host, &resolved_addr, XR_AF_UNSPEC)) {
        err = WS_ERR_DNS;
        goto fail_early;
    }

    // Create socket
    ws->fd = socket(resolved_addr.family, SOCK_STREAM, 0);
    if (ws->fd < 0) {
        err = WS_ERR_CONNECT;
        goto fail_early;
    }

    // Set TCP_NODELAY + low-latency options
    xr_socket_set_nodelay(ws->fd, true);
#ifdef TCP_NOTSENT_LOWAT
    int lowat = 16384;
    setsockopt(ws->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, (const char *) &lowat, sizeof(lowat));
#endif
#ifdef SO_NOSIGPIPE
    int flag = 1;
    setsockopt(ws->fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *) &flag, sizeof(flag));
#endif

    /*
     * Make the socket non-blocking BEFORE connect(). The old code
     * called connect() blocking and only set O_NONBLOCK after the
     * WS handshake finished (see the post-handshake fcntl below),
     * which meant every WS client connect pinned the worker thread
     * for the full TCP handshake + connect_timeout_ms — a problem
     * under high concurrency or unreachable hosts.
     *
     * The new flow:
     *   1. O_NONBLOCK on the fd.
     *   2. connect() returns -1 with EINPROGRESS for the common case
     *      (remote handshake in flight) or 0 on instant success
     *      (loopback, already-established).
     *   3. When EINPROGRESS, we yield via xr_socket_wait_writable —
     *      the coroutine suspends, the worker picks up other work,
     *      and netpoll wakes us when the socket is writable or the
     *      connect_timeout_ms deadline fires.
     *   4. Readable-via-SO_ERROR check distinguishes genuine
     *      success from an async connect failure (ECONNREFUSED,
     *      EHOSTUNREACH, etc.).
     */
    { (void) xr_socket_set_nonblocking(ws->fd); }

    // Connect
    struct sockaddr *sa;
    socklen_t sa_len;
    if (resolved_addr.family == AF_INET) {
        resolved_addr.addr.v4.sin_port = htons(ws->port);
        sa = (struct sockaddr *) &resolved_addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        resolved_addr.addr.v6.sin6_port = htons(ws->port);
        sa = (struct sockaddr *) &resolved_addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }

    int cret = connect(ws->fd, sa, sa_len);
    if (cret < 0 && errno != EINPROGRESS) {
        // Immediate failure (e.g. ECONNREFUSED on loopback). No point
        // yielding — report and bail.
        err = WS_ERR_CONNECT;
        goto fail_cleanup;
    }

    if (cret < 0) {
        /*
         * EINPROGRESS path: yield via netpoll until socket is writable
         * or the per-config connect_timeout_ms fires.
         */
        int connect_timeout = ws->config.connect_timeout_ms > 0 ? ws->config.connect_timeout_ms
                                                                : 10000;  // match cluster default

        XR_DCHECK(ws->isolate != NULL, "ws: isolate required for connect");
        int wr = xr_socket_wait_writable(ws->isolate, ws->fd, connect_timeout);
        if (wr <= 0) {
            err = (wr == 0) ? WS_ERR_TIMEOUT : WS_ERR_CONNECT;
            goto fail_cleanup;
        }

        // The socket became writable — pull SO_ERROR to distinguish
        // async-success from async-failure (peer RST after SYN etc.).
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(ws->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0 || so_error != 0) {
            err = WS_ERR_CONNECT;
            goto fail_cleanup;
        }
    }

    // Establish TLS connection if wss://
    if (ws->is_secure) {
        XrTlsContext *tls_ctx = xr_tls_context_new_client();
        if (!tls_ctx) {
            err = WS_ERR_CONNECT;
            goto fail_cleanup;
        }
        ws->tls_ctx = tls_ctx;

        XrTlsConn *tls_conn = xr_tls_conn_new(tls_ctx, ws->fd);
        if (!tls_conn) {
            err = WS_ERR_CONNECT;
            goto fail_cleanup;
        }
        ws->tls_conn = tls_conn;

        xr_tls_conn_set_hostname(tls_conn, ws->host);

        XrTlsError tls_err = xr_tls_conn_handshake_client(ws->isolate, tls_conn);
        if (tls_err != XR_TLS_OK) {
            err = WS_ERR_HANDSHAKE;
            goto fail_cleanup;
        }
    }

    // Generate Sec-WebSocket-Key
    ws->sec_key = generate_sec_key();
    if (!ws->sec_key) {
        err = WS_ERR_MEMORY;
        goto fail_cleanup;
    }

    // Build handshake request (dynamic buffer for large header sets)
    size_t req_cap = 2048;
    request = (char *) xr_malloc(req_cap);
    if (!request) {
        err = WS_ERR_MEMORY;
        goto fail_cleanup;
    }
    size_t req_len = 0;

#define WS_APPEND(fmt, ...)                                                                        \
    do {                                                                                           \
        int _n = snprintf(request + req_len, req_cap - req_len, fmt, ##__VA_ARGS__);               \
        if (_n < 0) {                                                                              \
            err = WS_ERR_MEMORY;                                                                   \
            goto fail_cleanup;                                                                     \
        }                                                                                          \
        if (req_len + (size_t) _n >= req_cap) {                                                    \
            size_t new_cap = (req_cap + (size_t) _n + 1) * 2;                                      \
            char *tmp = (char *) xr_realloc(request, new_cap);                                     \
            if (!tmp) {                                                                            \
                err = WS_ERR_MEMORY;                                                               \
                goto fail_cleanup;                                                                 \
            }                                                                                      \
            request = tmp;                                                                         \
            req_cap = new_cap;                                                                     \
            _n = snprintf(request + req_len, req_cap - req_len, fmt, ##__VA_ARGS__);               \
            if (_n < 0) {                                                                          \
                err = WS_ERR_MEMORY;                                                               \
                goto fail_cleanup;                                                                 \
            }                                                                                      \
        }                                                                                          \
        req_len += (size_t) _n;                                                                    \
    } while (0)

    WS_APPEND("GET %s HTTP/1.1\r\n", ws->path);
    WS_APPEND("Host: %s", ws->host);
    if ((ws->is_secure && ws->port != 443) || (!ws->is_secure && ws->port != 80)) {
        WS_APPEND(":%d", ws->port);
    }
    WS_APPEND("\r\n");
    WS_APPEND("Upgrade: websocket\r\n");
    WS_APPEND("Connection: Upgrade\r\n");
    WS_APPEND("Sec-WebSocket-Key: %s\r\n", ws->sec_key);
    WS_APPEND("Sec-WebSocket-Version: 13\r\n");
    WS_APPEND("Sec-WebSocket-Extensions: permessage-deflate; "
              "client_no_context_takeover; server_no_context_takeover\r\n");

    for (int i = 0; i + 1 < ws->config.header_count; i += 2) {
        WS_APPEND("%s: %s\r\n", ws->config.headers[i], ws->config.headers[i + 1]);
    }

    if (ws->config.subprotocols && ws->config.subprotocol_count > 0) {
        WS_APPEND("Sec-WebSocket-Protocol: ");
        for (int i = 0; i < ws->config.subprotocol_count; i++) {
            if (i > 0)
                WS_APPEND(", ");
            WS_APPEND("%s", ws->config.subprotocols[i]);
        }
        WS_APPEND("\r\n");
    }

    WS_APPEND("\r\n");
#undef WS_APPEND

    // Send handshake request
    if (ws_send_all(ws, request, req_len) < 0) {
        err = WS_ERR_SEND;
        goto fail_cleanup;
    }
    xr_free(request);
    request = NULL;

    // Receive handshake response (dynamic buffer)
    size_t resp_cap = 4096;
    response = (char *) xr_malloc(resp_cap);
    if (!response) {
        err = WS_ERR_MEMORY;
        goto fail_cleanup;
    }
    size_t resp_len = 0;

    while (resp_len < resp_cap - 1) {
        ssize_t n = ws_recv_timeout(ws, response + resp_len, resp_cap - 1 - resp_len,
                                    ws->config.connect_timeout_ms);
        if (n <= 0) {
            err = WS_ERR_RECV;
            goto fail_cleanup;
        }
        resp_len += n;
        response[resp_len] = '\0';
        if (strstr(response, "\r\n\r\n"))
            break;
    }

    // Validate response
    if (strncmp(response, "HTTP/1.1 101", 12) != 0) {
        err = WS_ERR_HANDSHAKE;
        goto fail_cleanup;
    }

    // Validate Sec-WebSocket-Accept with strict exact-match against the
    // full trimmed header value. The old `strstr(response, accept_key)` was
    // a substring scan over the whole response; any header whose value
    // happened to contain the base64 digest (e.g. custom diagnostic echo,
    // proxy inserted fields) would have been accepted (W-12).
    accept_key = compute_accept_key(ws->sec_key);
    if (!accept_key) {
        err = WS_ERR_MEMORY;
        goto fail_cleanup;
    }

    char accept_val[64];
    if (!ws_get_header_value(response, "Sec-WebSocket-Accept", accept_val, sizeof(accept_val))) {
        err = WS_ERR_HANDSHAKE;
        goto fail_cleanup;
    }
    if (strcmp(accept_val, accept_key) != 0) {
        err = WS_ERR_HANDSHAKE;
        goto fail_cleanup;
    }
    xr_free(accept_key);
    accept_key = NULL;

    // Parse Sec-WebSocket-Extensions to detect permessage-deflate. The old
    // `strstr(response, "permessage-deflate")` also triggered on cookie
    // values, custom headers, error strings — even if the server did not
    // actually negotiate the extension (W-13). Now we walk the header's
    // comma-separated token list and match only the `name` part.
    {
        char ext_val[512];
        if (ws_get_header_value(response, "Sec-WebSocket-Extensions", ext_val, sizeof(ext_val))) {
            if (ws_header_list_has_token(ext_val, "permessage-deflate")) {
                ws->deflate_enabled = true;
                ws->deflate_no_context = true;  // we requested no_context_takeover
            }
        }
    }

    /*
     * Re-assert O_NONBLOCK defensively. The socket was already set
     * non-blocking before the TCP connect() in the updated client
     * flow (see the pre-connect fcntl block above), so this is
     * normally a no-op. Kept in place because some TLS client
     * libraries (rare) temporarily toggle the flag during their
     * own SSL_connect plumbing; re-arming here guarantees the
     * post-handshake read/write paths consistently yield via
     * netpoll instead of blocking the worker thread.
     */
    xr_socket_set_nonblocking(ws->fd);

    // Connection successful
    ws->state = WS_STATE_OPEN;
    xr_free(response);

    return WS_OK;

fail_cleanup:
    xr_free(accept_key);
    xr_free(request);
    xr_free(response);
    if (ws->tls_conn) {
        xr_tls_conn_close((XrTlsConn *) ws->tls_conn);
        xr_tls_conn_free((XrTlsConn *) ws->tls_conn);
        ws->tls_conn = NULL;
    }
    if (ws->tls_ctx) {
        xr_tls_context_free((XrTlsContext *) ws->tls_ctx);
        ws->tls_ctx = NULL;
    }
    if (ws->fd >= 0) {
        xr_closesocket(ws->fd);
        ws->fd = -1;
    }
fail_early:
    ws->state = WS_STATE_CLOSED;
    return err;
}

XrWsError xr_ws_close(XrWebSocket *ws, int code, const char *reason) {
    if (!ws || ws->state == WS_STATE_CLOSED)
        return WS_OK;

    if (ws->state == WS_STATE_OPEN) {
        ws->state = WS_STATE_CLOSING;

        // Send close frame
        char close_data[128];
        size_t close_len = 0;

        if (code > 0) {
            close_data[0] = (char) ((code >> 8) & 0xFF);
            close_data[1] = (char) (code & 0xFF);
            close_len = 2;

            if (reason) {
                size_t reason_len = strlen(reason);
                if (reason_len > sizeof(close_data) - 2) {
                    reason_len = sizeof(close_data) - 2;
                }
                memcpy(close_data + 2, reason, reason_len);
                close_len += reason_len;
            }
        }

        // RFC 6455: Client MUST mask, server MUST NOT mask
        bool mask = !ws->is_server;

        send_close_frame(ws, close_data, close_len, mask);

        ws->close_code = code;
        if (reason) {
            ws->close_reason = xr_strdup(reason);
        }
    }

    // Close TLS connection first
    if (ws->tls_conn) {
        xr_tls_conn_close((XrTlsConn *) ws->tls_conn);
        xr_tls_conn_free((XrTlsConn *) ws->tls_conn);
        ws->tls_conn = NULL;
    }
    if (ws->tls_ctx) {
        xr_tls_context_free((XrTlsContext *) ws->tls_ctx);
        ws->tls_ctx = NULL;
    }

    if (ws->fd >= 0) {
        xr_closesocket(ws->fd);
        ws->fd = -1;
    }

    ws->state = WS_STATE_CLOSED;

    return WS_OK;
}

// Send a compressed frame: deflate payload, set RSV1 bit.
// On compression failure this reports an error rather than falling back
// to an uncompressed frame: the peer negotiated deflate and expects RSV1=1,
// so sending a raw frame would be a protocol error on the wire.
static int ws_send_frame_compressed(XrWebSocket *ws, XrWsOpcode opcode, const void *data,
                                    size_t len) {
    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    if (xr_ws_deflate_compress((const uint8_t *) data, len, &compressed, &compressed_len) != 0) {
        return -1;
    }

    // Build header with RSV1 bit set
    char header[WS_MAX_HEADER_SIZE];
    unsigned char mask_key[4] = {0};
    bool mask = !ws->is_server;
    size_t header_len = build_frame_header(header, opcode, compressed_len, mask, mask_key, true);

    int ret;
    if (!mask) {
        // Server: header + compressed payload
        if (ws_send_all(ws, header, header_len) < 0 ||
            ws_send_all(ws, compressed, compressed_len) < 0) {
            ret = -1;
        } else {
            ret = 0;
        }
    } else {
        // Client: mask then send
        size_t frame_len = header_len + compressed_len;
        char *frame = (char *) xr_malloc(frame_len);
        if (!frame) {
            xr_free(compressed);
            return -1;
        }
        memcpy(frame, header, header_len);
        apply_mask(compressed, (unsigned char *) (frame + header_len), compressed_len, mask_key);
        ret = ws_send_all(ws, frame, frame_len) < 0 ? -1 : 0;
        xr_free(frame);
    }
    xr_free(compressed);
    return ret;
}

XrWsError xr_ws_send_text(XrWebSocket *ws, const char *text, size_t len) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return WS_ERR_CLOSED;
    if (!text)
        return WS_ERR_SEND;

    if (len == 0)
        len = strlen(text);

    int ret;
    if (ws->deflate_enabled && len > 0) {
        ret = ws_send_frame_compressed(ws, WS_OPCODE_TEXT, text, len);
    } else {
        ret = ws_send_frame_writev(ws, WS_OPCODE_TEXT, text, len);
    }

    return ret < 0 ? WS_ERR_SEND : WS_OK;
}

XrWsError xr_ws_send_binary(XrWebSocket *ws, const void *data, size_t len) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return WS_ERR_CLOSED;
    if (!data || len == 0)
        return WS_ERR_SEND;

    int ret;
    if (ws->deflate_enabled) {
        ret = ws_send_frame_compressed(ws, WS_OPCODE_BINARY, data, len);
    } else {
        ret = ws_send_frame_writev(ws, WS_OPCODE_BINARY, data, len);
    }

    return ret < 0 ? WS_ERR_SEND : WS_OK;
}

XrWsError xr_ws_ping(XrWebSocket *ws) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return WS_ERR_CLOSED;

    // Use optimized send for control frame
    int ret = ws_send_frame_writev(ws, WS_OPCODE_PING, NULL, 0);

    return ret < 0 ? WS_ERR_SEND : WS_OK;
}

XrWsError xr_ws_pong(XrWebSocket *ws, const void *data, size_t len) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return WS_ERR_CLOSED;

    // Use optimized send for control frame
    int ret = ws_send_frame_writev(ws, WS_OPCODE_PONG, data, len);

    return ret < 0 ? WS_ERR_SEND : WS_OK;
}

/*
 * Blocking-style recv: loops xr_ws_recv_try + xr_socket_wait_readable.
 * Yields the coroutine on EAGAIN via netpoll. The 100ms slice keeps
 * bounded latency for concurrent xr_ws_close state transitions.
 * Requires ws->isolate.
 */
XrWsMessage *xr_ws_recv(XrWebSocket *ws) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return NULL;
    XR_DCHECK(ws->isolate != NULL, "ws: isolate required for recv");

    while (1) {
        bool need_more = false;
        XrWsMessage *msg = xr_ws_recv_try(ws, &need_more);

        if (msg)
            return msg;
        if (ws->state != WS_STATE_OPEN)
            return NULL;

        if (!need_more)
            continue;

        int wr = xr_socket_wait_readable(ws->isolate, ws->fd, 100);
        if (wr < 0) {
            xr_ws_close(ws, WS_CLOSE_ABNORMAL, NULL);
            return NULL;
        }
    }
}

// Fill rbuf from network, drain until EAGAIN (for edge-triggered kqueue)
static ssize_t ws_rbuf_fill(XrWebSocket *ws) {
    if (!ws_rbuf_ensure(ws))
        return -1;

    // Compact when consumed portion exceeds half capacity
    if (ws->rbuf_off > ws->rbuf_cap / 2)
        ws_rbuf_compact(ws);

    ssize_t total = 0;
    for (;;) {
        int avail = ws->rbuf_cap - ws->rbuf_off - ws->rbuf_len;
        if (avail <= 0)
            break;
        char *wp = ws->rbuf + ws->rbuf_off + ws->rbuf_len;
        ssize_t n;
        if (ws->is_secure && ws->tls_conn) {
            n = xr_tls_conn_read(ws->isolate, (XrTlsConn *) ws->tls_conn, wp, avail);
        } else {
            n = recv(ws->fd, wp, avail, 0);
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1;
        }
        if (n == 0) {
            if (total > 0)
                break;
            return -2;
        }
        ws->rbuf_len += (int) n;
        total += n;
    }
    return total;
}

// Non-blocking recv for yieldable integration
// Design: flat rbuf + dynamic msg_buf, zero spill buffer
// Returns message if complete, NULL if need_more or error
XrWsMessage *xr_ws_recv_try(XrWebSocket *ws, bool *need_more) {
    if (need_more)
        *need_more = false;
    if (!ws || ws->state != WS_STATE_OPEN)
        return NULL;

    // Declared at function scope so `goto read_payload` (bypasses parse_header)
    // and the subsequent process_frame path read a deterministic value.
    // Fast path later overwrites this to true when the payload lives in rbuf.
    bool payload_inplace = false;

    // Auto-ping / pong-timeout check (skipped when ping_interval_ms == 0)
    if (ws->config.ping_interval_ms > 0 && !ws->frame_in_progress) {
        uint64_t now = ws_now_ms();
        if (ws->ping_in_flight) {
            if (ws->config.pong_timeout_ms > 0 &&
                now - ws->last_ping_sent_ms > (uint64_t) ws->config.pong_timeout_ms) {
                xr_ws_close(ws, WS_CLOSE_GOING_AWAY, "pong timeout");
                return NULL;
            }
        } else {
            if (now - ws->last_pong_recv_ms > (uint64_t) ws->config.ping_interval_ms) {
                xr_ws_ping(ws);
                ws->last_ping_sent_ms = now;
                ws->ping_in_flight = true;
            }
        }
    }

    if (ws->frame_in_progress)
        goto read_payload;

    // If rbuf has data from previous drain, parse without recv
    if (ws->rbuf_len > 0)
        goto parse_header;

    // Read from network
    {
        ssize_t r = ws_rbuf_fill(ws);
        if (r == -1 || r == -2) {
            ws->state = WS_STATE_CLOSED;
            return NULL;
        }
    }

parse_header:;
    if (ws->rbuf_len < 2) {
        if (need_more)
            *need_more = true;
        return NULL;
    }

    // Parse frame header directly from rbuf (zero-copy, no spill)
    char *rp = ws->rbuf + ws->rbuf_off;

    bool fin = false;
    XrWsOpcode opcode = (XrWsOpcode) 0;
    bool masked = false;
    uint64_t payload_len = 0;
    unsigned char mask_key[4] = {0};
    size_t header_len = 0;
    bool frame_rsv1 = false;

    int parsed = parse_frame_header(rp, (size_t) ws->rbuf_len, &fin, &opcode, &masked, &payload_len,
                                    mask_key, &header_len, &frame_rsv1);
    if (parsed < 0) {
        if (ws->state == WS_STATE_OPEN) {
            char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF, WS_CLOSE_PROTOCOL_ERROR & 0xFF};
            send_close_frame(ws, cd, 2, !ws->is_server);
        }
        ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }
    if (!parsed) {
        // Incomplete header stays in rbuf, wait for more data
        if (need_more)
            *need_more = true;
        return NULL;
    }

    // RFC 6455 Section 5.1: server MUST close if client frame is not masked
    if (ws->is_server && !masked) {
        if (ws->state == WS_STATE_OPEN) {
            char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF, WS_CLOSE_PROTOCOL_ERROR & 0xFF};
            send_close_frame(ws, cd, 2, false);
        }
        ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    // RFC 6455 Section 5.1: client MUST close if server frame is masked
    if (!ws->is_server && masked) {
        ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    // Check max_message_size BEFORE allocating buffer
    size_t max_size = ws->config.max_message_size;
    if (max_size == 0)
        max_size = WS_DEFAULT_MAX_MESSAGE_SIZE;
    if (payload_len > max_size) {
        if (ws->state == WS_STATE_OPEN) {
            char cd[2] = {(WS_CLOSE_TOO_LARGE >> 8) & 0xFF, WS_CLOSE_TOO_LARGE & 0xFF};
            send_close_frame(ws, cd, 2, !ws->is_server);
        }
        ws->close_code = WS_CLOSE_TOO_LARGE;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    // Consume header from rbuf
    ws_rbuf_consume(ws, (int) header_len);

    // Save frame state
    ws->frame_fin = fin;
    ws->frame_opcode = opcode;
    ws->frame_masked = masked;
    ws->frame_rsv1 = frame_rsv1;
    if (masked)
        memcpy(ws->frame_mask, mask_key, 4);

    // Payload processing
    char *payload;
    size_t payload_len_actual;
    // payload_inplace declared at function scope above; reset for new frame.
    payload_inplace = false;

    // Fast path: payload fits entirely in rbuf -> in-place unmask, no malloc
    if (payload_len > 0 && payload_len <= (size_t) ws->rbuf_len) {
        payload = ws->rbuf + ws->rbuf_off;
        payload_len_actual = payload_len;
        if (masked) {
            unmask_inplace((unsigned char *) payload, payload_len_actual, mask_key);
        }
        payload_inplace = true;
        ws_rbuf_consume(ws, (int) payload_len);
        goto process_frame;
    }

    // Slow path: allocate msg_buf for payload
    if (payload_len > 0) {
        if (ws->msg_buf_size < payload_len) {
            xr_free(ws->msg_buf);
            ws->msg_buf = (char *) xr_malloc(payload_len + 1);
            if (!ws->msg_buf) {
                ws->msg_buf_size = 0;
                ws->state = WS_STATE_CLOSED;
                return NULL;
            }
            ws->msg_buf_size = payload_len + 1;
        }
        ws->msg_buf_len = 0;
        ws->msg_remaining = payload_len;
        ws->frame_in_progress = true;
    } else {
        ws->msg_remaining = 0;
        ws->msg_buf_len = 0;
        ws->frame_in_progress = false;
    }

read_payload:
    while (ws->msg_remaining > 0) {
        // Drain rbuf first
        if (ws->rbuf_len > 0) {
            size_t copy = ((size_t) ws->rbuf_len < ws->msg_remaining) ? (size_t) ws->rbuf_len
                                                                      : ws->msg_remaining;
            memcpy(ws->msg_buf + ws->msg_buf_len, ws->rbuf + ws->rbuf_off, copy);
            ws->msg_buf_len += copy;
            ws->msg_remaining -= copy;
            ws_rbuf_consume(ws, (int) copy);
        }

        if (ws->msg_remaining == 0)
            break;

        // Large payload: recv directly into msg_buf, bypassing rbuf
        if (ws->msg_remaining > WS_RBUF_INIT_CAP) {
            size_t direct_max = ws->msg_remaining - WS_RBUF_INIT_CAP / 2;
            ssize_t n;
            if (ws->is_secure && ws->tls_conn) {
                n = xr_tls_conn_read(ws->isolate, (XrTlsConn *) ws->tls_conn,
                                     ws->msg_buf + ws->msg_buf_len, direct_max);
            } else {
                n = recv(ws->fd, ws->msg_buf + ws->msg_buf_len, direct_max, 0);
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (need_more)
                        *need_more = true;
                    return NULL;
                }
                ws->frame_in_progress = false;
                ws->state = WS_STATE_CLOSED;
                return NULL;
            }
            if (n == 0) {
                ws->frame_in_progress = false;
                ws->state = WS_STATE_CLOSED;
                return NULL;
            }
            ws->msg_buf_len += n;
            ws->msg_remaining -= n;
            continue;
        }

        // Small remaining: fill rbuf (drains edge-triggered socket)
        ssize_t r = ws_rbuf_fill(ws);
        if (r == -1 || r == -2) {
            ws->frame_in_progress = false;
            ws->state = WS_STATE_CLOSED;
            return NULL;
        }
        if (ws->rbuf_len == 0) {
            if (need_more)
                *need_more = true;
            return NULL;
        }
    }

    // Frame payload complete
    ws->frame_in_progress = false;
    payload = ws->msg_buf;
    payload_len_actual = ws->msg_buf_len;

    if (ws->frame_masked && payload_len_actual > 0) {
        unmask_inplace((unsigned char *) payload, payload_len_actual, ws->frame_mask);
    }

process_frame:
    // Handle control frames
    if (ws->frame_opcode == WS_OPCODE_CLOSE) {
        if (payload_len_actual == 1) {
            if (ws->state == WS_STATE_OPEN) {
                char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF,
                              WS_CLOSE_PROTOCOL_ERROR & 0xFF};
                send_close_frame(ws, cd, 2, !ws->is_server);
            }
            ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
            ws->state = WS_STATE_CLOSED;
            return NULL;
        }
        int code = WS_CLOSE_NO_STATUS;
        if (payload_len_actual >= 2) {
            code = ((unsigned char) payload[0] << 8) | (unsigned char) payload[1];
            bool valid = (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
                          code == 1007 || code == 1008 || code == 1009 || code == 1010 ||
                          code == 1011 || (code >= 3000 && code <= 4999));
            if (!valid) {
                if (ws->state == WS_STATE_OPEN) {
                    char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF,
                                  WS_CLOSE_PROTOCOL_ERROR & 0xFF};
                    send_close_frame(ws, cd, 2, !ws->is_server);
                }
                ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
                ws->state = WS_STATE_CLOSED;
                return NULL;
            }
        }
        if (payload_len_actual > 2) {
            if (!xr_utf8_validate(payload + 2, payload_len_actual - 2)) {
                if (ws->state == WS_STATE_OPEN) {
                    char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF,
                                  WS_CLOSE_PROTOCOL_ERROR & 0xFF};
                    send_close_frame(ws, cd, 2, !ws->is_server);
                }
                ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
                ws->state = WS_STATE_CLOSED;
                return NULL;
            }
        }
        if (ws->state == WS_STATE_OPEN) {
            ws->state = WS_STATE_CLOSING;
            bool mask = !ws->is_server;
            if (payload_len_actual >= 2) {
                send_close_frame(ws, payload, payload_len_actual, mask);
            } else {
                send_close_frame(ws, NULL, 0, mask);
            }
        }
        ws->close_code = code;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    if (ws->frame_opcode == WS_OPCODE_PING) {
        xr_ws_pong(ws, payload, payload_len_actual);
        if (ws->rbuf_len > 0)
            goto parse_header;
        if (need_more)
            *need_more = true;
        return NULL;
    }

    if (ws->frame_opcode == WS_OPCODE_PONG) {
        ws->last_pong_recv_ms = ws_now_ms();
        ws->ping_in_flight = false;
        if (ws->rbuf_len > 0)
            goto parse_header;
        if (need_more)
            *need_more = true;
        return NULL;
    }

    // Handle data frames
    XrWsMessage *msg = &ws->last_msg;
    msg->data = NULL;
    msg->len = 0;
    msg->_flags = XR_WS_MSG_NO_FREE;

    // RFC 6455 Section 5.4: continuation without in-progress fragmentation
    if (ws->frame_opcode == WS_OPCODE_CONTINUATION && !ws->frag_buf) {
        if (ws->state == WS_STATE_OPEN) {
            char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF, WS_CLOSE_PROTOCOL_ERROR & 0xFF};
            send_close_frame(ws, cd, 2, !ws->is_server);
        }
        ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    // RFC 6455 Section 5.4: new data frame while fragmentation is in progress
    if (ws->frame_opcode != WS_OPCODE_CONTINUATION && ws->frag_buf) {
        if (ws->state == WS_STATE_OPEN) {
            char cd[2] = {(WS_CLOSE_PROTOCOL_ERROR >> 8) & 0xFF, WS_CLOSE_PROTOCOL_ERROR & 0xFF};
            send_close_frame(ws, cd, 2, !ws->is_server);
        }
        ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
        ws->state = WS_STATE_CLOSED;
        return NULL;
    }

    if (ws->frame_opcode == WS_OPCODE_CONTINUATION && ws->frag_buf) {
        if (ws_frag_append(ws, payload, payload_len_actual) < 0) {
            return NULL;
        }
        if (ws->frame_fin) {
            ws_frag_complete(ws, msg);
        } else {
            if (ws->rbuf_len > 0)
                goto parse_header;
            if (need_more)
                *need_more = true;
            return NULL;
        }
    } else if (!ws->frame_fin) {
        if (ws_frag_start(ws, ws->frame_opcode, payload, payload_len_actual) < 0) {
            return NULL;
        }
        if (ws->rbuf_len > 0)
            goto parse_header;
        if (need_more)
            *need_more = true;
        return NULL;
    } else {
        // Complete single-frame message
        msg->opcode = ws->frame_opcode;
        if (payload_inplace && payload_len_actual > 0) {
            // Zero-copy: data points into rbuf (already unmasked in-place)
            msg->data = payload;
            msg->_flags |= XR_WS_MSG_DATA_INPLACE;
        } else if (ws->msg_buf && payload_len_actual > 0) {
            msg->data = ws->msg_buf;
            ws->msg_buf = NULL;
            ws->msg_buf_size = 0;
        } else {
            msg->data = (char *) xr_calloc(1, 1);
        }
        msg->len = payload_len_actual;
        msg->is_text = (ws->frame_opcode == WS_OPCODE_TEXT);
    }

    // permessage-deflate: decompress if RSV1 was set on the data frame.
    // Read from ws->frame_rsv1 because we may have re-entered via the
    // `goto read_payload` path which skips the local `frame_rsv1` decl.
    if (ws->frame_rsv1 && ws->deflate_enabled && msg->data && msg->len > 0) {
        uint8_t *decompressed = NULL;
        size_t decompressed_len = 0;
        // Zip-bomb guard: bound decompressed output by max_message_size.
        // RFC 7692 has no transport-level size limit, so we reuse the WS
        // frame budget which is also the fragmented-message limit.
        size_t max_out = ws->config.max_message_size;
        if (max_out == 0)
            max_out = WS_DEFAULT_MAX_MESSAGE_SIZE;
        if (xr_ws_deflate_decompress((const uint8_t *) msg->data, msg->len, max_out, &decompressed,
                                     &decompressed_len) == 0) {
            if (!(msg->_flags & XR_WS_MSG_DATA_INPLACE))
                xr_free(msg->data);
            msg->data = (char *) decompressed;
            msg->len = decompressed_len;
            msg->_flags &= (uint8_t) ~XR_WS_MSG_DATA_INPLACE;
        } else {
            // Decompression failed or exceeded size budget.
            // Treat as protocol / policy error and close connection.
            if (!(msg->_flags & XR_WS_MSG_DATA_INPLACE))
                xr_free(msg->data);
            msg->data = NULL;
            msg->len = 0;
            msg->_flags &= (uint8_t) ~XR_WS_MSG_DATA_INPLACE;
            if (ws->state == WS_STATE_OPEN) {
                char cd[2] = {(WS_CLOSE_TOO_LARGE >> 8) & 0xFF, WS_CLOSE_TOO_LARGE & 0xFF};
                send_close_frame(ws, cd, 2, !ws->is_server);
            }
            ws->close_code = WS_CLOSE_TOO_LARGE;
            ws->state = WS_STATE_CLOSED;
            return NULL;
        }
    }

    // RFC 6455 Section 5.6: text frames MUST be valid UTF-8
    if (msg->is_text && msg->len > 0) {
        if (!xr_utf8_validate(msg->data, msg->len)) {
            if (!(msg->_flags & XR_WS_MSG_DATA_INPLACE))
                xr_free(msg->data);
            msg->data = NULL;
            msg->_flags &= (uint8_t) ~XR_WS_MSG_DATA_INPLACE;
            if (ws->state == WS_STATE_OPEN) {
                char cd[2] = {(WS_CLOSE_INVALID_DATA >> 8) & 0xFF, WS_CLOSE_INVALID_DATA & 0xFF};
                send_close_frame(ws, cd, 2, !ws->is_server);
            }
            ws->close_code = WS_CLOSE_INVALID_DATA;
            ws->state = WS_STATE_CLOSED;
            return NULL;
        }
    }

    return msg;
}

void xr_ws_cork(XrWebSocket *ws) {
    if (!ws)
        return;
    ws->corked = true;
    ws->wbuf_len = 0;  // reset write position (keep allocated buffer)
}

int xr_ws_uncork(XrWebSocket *ws) {
    if (!ws)
        return -1;
    ws->corked = false;
    if (ws->wbuf_len == 0)
        return 0;  // nothing buffered

    // Single non-blocking send for all corked frames
    int total = ws->wbuf_len;
    ws->wbuf_len = 0;

    if (ws->is_secure && ws->tls_conn) {
        return ws_send_all(ws, ws->wbuf, total);
    }

    ssize_t n = send(ws->fd, ws->wbuf, total, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            n = 0;
        else
            return -1;
    }
    if ((int) n >= total)
        return 0;

    // Partial send: blocking flush for remainder
    if (ws_send_all(ws, ws->wbuf + n, total - n) < 0)
        return -1;
    return 0;
}

int xr_ws_poll(XrWebSocket *ws, int timeout_ms) {
    if (!ws || ws->state != WS_STATE_OPEN)
        return -1;
    XR_DCHECK(ws->isolate != NULL, "ws: isolate required for poll");
    return xr_socket_wait_readable(ws->isolate, ws->fd, timeout_ms);
}

void xr_ws_message_free(XrWsMessage *msg) {
    if (!msg)
        return;
    if (!(msg->_flags & XR_WS_MSG_DATA_INPLACE)) {
        xr_free(msg->data);
    }
    msg->data = NULL;
    msg->_flags &= (uint8_t) ~XR_WS_MSG_DATA_INPLACE;
    if (!(msg->_flags & XR_WS_MSG_NO_FREE))
        xr_free(msg);
}

void xr_ws_message_recycle(XrWebSocket *ws, XrWsMessage *msg) {
    if (!msg || !msg->data)
        return;
    if (msg->_flags & XR_WS_MSG_DATA_INPLACE) {
        // inplace data lives in rbuf, nothing to recycle
        msg->data = NULL;
        return;
    }
    // Return heap buffer to ws->msg_buf for reuse by next recv_try
    if (!ws->msg_buf) {
        ws->msg_buf = msg->data;
        ws->msg_buf_size = msg->len + 1;
    } else if (msg->len + 1 > ws->msg_buf_size) {
        // Incoming buffer is larger — swap to keep the bigger one
        xr_free(ws->msg_buf);
        ws->msg_buf = msg->data;
        ws->msg_buf_size = msg->len + 1;
    } else {
        xr_free(msg->data);
    }
    msg->data = NULL;
}

XrWsState xr_ws_get_state(XrWebSocket *ws) {
    return ws ? ws->state : WS_STATE_CLOSED;
}

const char *xr_ws_error_string(XrWsError err) {
    return xr_net_error_string(err);
}

/* ========== WebSocket Server Implementation ========== */

// Case-insensitive HTTP header search (RFC 7230: header names are case-insensitive)
static const char *ws_find_header(const char *headers, const char *name) {
    if (!headers || !name)
        return NULL;
    size_t name_len = strlen(name);
    const char *p = headers;
    while (*p) {
        // Match at start of line (case-insensitive)
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            return p;
        }
        // Advance to next line
        const char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    return NULL;
}

/*
 * Copy the value of `name` from an HTTP header blob into `out` (bounded,
 * NUL-terminated, trimmed). Returns true if header was found and fit.
 *
 * Used to replace the previous `strstr(response, "...")` pattern that
 * accepted a substring appearing anywhere in the message, including
 * inside another header's value (W-12 / W-13).
 */
static bool ws_get_header_value(const char *headers, const char *name, char *out, size_t out_size) {
    if (!headers || !name || !out || out_size == 0)
        return false;
    const char *h = ws_find_header(headers, name);
    if (!h)
        return false;
    h += strlen(name);
    if (*h != ':')
        return false;
    h++;
    while (*h == ' ' || *h == '\t')
        h++;

    const char *end = strchr(h, '\r');
    if (!end)
        end = strchr(h, '\n');
    if (!end)
        end = h + strlen(h);

    // Trim trailing whitespace.
    while (end > h && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    size_t len = (size_t) (end - h);
    if (len >= out_size)
        return false;
    memcpy(out, h, len);
    out[len] = '\0';
    return true;
}

/*
 * Case-insensitive match of a token within a comma-separated extension /
 * subprotocol header value. Each token is `name[; params...]`; we only
 * compare the `name` part and ignore surrounding whitespace.
 *
 * Example: ws_header_list_has("chat, permessage-deflate; client_no_context_takeover",
 *                             "permessage-deflate") → true
 *
 * This replaces naïve `strstr` scans that would falsely match when the
 * token name appears inside another header's value (W-13).
 */
static bool ws_header_list_has_token(const char *value, const char *token) {
    if (!value || !token)
        return false;
    size_t token_len = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            return false;
        const char *tok = p;
        while (*p && *p != ',' && *p != ';')
            p++;
        const char *tok_end = p;
        while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) {
            tok_end--;
        }
        size_t len = (size_t) (tok_end - tok);
        if (len == token_len && strncasecmp(tok, token, token_len) == 0) {
            return true;
        }
        // Skip params up to next comma.
        while (*p && *p != ',')
            p++;
    }
    return false;
}

bool xr_ws_is_upgrade_request(const char *request_headers) {
    if (!request_headers)
        return false;

    const char *upgrade = ws_find_header(request_headers, "Upgrade");
    if (!upgrade)
        return false;

    // Extract header value and check for "websocket" (case-insensitive)
    const char *val = upgrade + 8;  // strlen("Upgrade:")
    while (*val == ' ' || *val == '\t')
        val++;

    const char *line_end = strchr(val, '\r');
    if (!line_end)
        line_end = strchr(val, '\n');
    if (!line_end)
        return false;

    size_t val_len = line_end - val;
    char buf[256];
    if (val_len >= sizeof(buf))
        return false;
    memcpy(buf, val, val_len);
    buf[val_len] = '\0';

    for (char *c = buf; *c; c++) {
        if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    }

    return strstr(buf, "websocket") != NULL;
}

char *xr_ws_get_sec_key(const char *request_headers) {
    if (!request_headers)
        return NULL;

    const char *key_header = ws_find_header(request_headers, "Sec-WebSocket-Key");
    if (!key_header)
        return NULL;

    // Skip header name + ':'
    key_header += 18;  // strlen("Sec-WebSocket-Key:")
    while (*key_header == ' ' || *key_header == '\t')
        key_header++;

    // Find end of line
    const char *line_end = strchr(key_header, '\r');
    if (!line_end)
        line_end = strchr(key_header, '\n');
    if (!line_end)
        return NULL;

    size_t key_len = line_end - key_header;
    char *key = (char *) xr_malloc(key_len + 1);
    if (!key)
        return NULL;

    memcpy(key, key_header, key_len);
    key[key_len] = '\0';

    // Trim trailing whitespace
    while (key_len > 0 && (key[key_len - 1] == ' ' || key[key_len - 1] == '\t')) {
        key[--key_len] = '\0';
    }

    return key;
}

int xr_ws_send_upgrade_response(int fd, const char *sec_key, const char *protocol,
                                bool deflate_ok) {
    if (fd < 0 || !sec_key)
        return -1;

    // Calculate Sec-WebSocket-Accept
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", sec_key, WS_GUID);

    unsigned char hash[20];
    SHA1((unsigned char *) concat, strlen(concat), hash);

    char *accept_key = xr_base64_encode(hash, 20, NULL);
    if (!accept_key)
        return -1;

    // Build response
    char response[1024];
    int len;

    const char *ext_hdr = deflate_ok ? "Sec-WebSocket-Extensions: permessage-deflate; "
                                       "server_no_context_takeover; client_no_context_takeover\r\n"
                                     : "";

    if (protocol && protocol[0]) {
        len = snprintf(response, sizeof(response),
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n"
                       "Sec-WebSocket-Protocol: %s\r\n"
                       "%s"
                       "\r\n",
                       accept_key, protocol, ext_hdr);
    } else {
        len = snprintf(response, sizeof(response),
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n"
                       "%s"
                       "\r\n",
                       accept_key, ext_hdr);
    }

    xr_free(accept_key);

    // Send response (retry on EINTR)
    size_t total_sent = 0;
    while (total_sent < (size_t) len) {
        ssize_t n = write(fd, response + total_sent, len - total_sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total_sent += n;
    }
    return 0;
}

/*
 * Extract the `name` part of each comma-separated token in a subprotocol
 * header value. Used by xr_ws_pick_subprotocol; RFC 6455 requires that
 * the server picks one name from the client's offer list, case-sensitive.
 */
static bool ws_origin_allowed(const char *origin, const char **allowlist) {
    if (!allowlist || !allowlist[0])
        return true;
    if (!origin || !origin[0]) {
        // No Origin sent by client. Reject when an allowlist is configured
        // to avoid non-browser clients bypassing the policy; callers who
        // want to accept missing Origin can opt in via "*".
        for (const char **p = allowlist; *p; p++) {
            if (strcmp(*p, "*") == 0)
                return true;
        }
        return false;
    }
    for (const char **p = allowlist; *p; p++) {
        if (strcmp(*p, "*") == 0)
            return true;
        if (strcmp(*p, origin) == 0)
            return true;
    }
    return false;
}

/*
 * Send a minimal HTTP error response (used for Origin rejection) then
 * return. Caller is responsible for closing fd.
 */
static void ws_send_simple_response(int fd, int status, const char *reason) {
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason ? reason : "");
    if (n > 0) {
        ssize_t w = write(fd, resp, (size_t) n);
        (void) w;
    }
}

char *xr_ws_pick_subprotocol(const char *request_headers, const char **server_protocols) {
    if (!request_headers || !server_protocols || !server_protocols[0]) {
        return NULL;
    }
    char offer[512];
    if (!ws_get_header_value(request_headers, "Sec-WebSocket-Protocol", offer, sizeof(offer))) {
        return NULL;
    }
    // Walk the client's offer in order; RFC 6455 lets the server freely
    // pick any one of the offered names, but clients typically list them
    // by preference so we honour that.
    const char *p = offer;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *tok = p;
        while (*p && *p != ',' && *p != ';')
            p++;
        const char *tok_end = p;
        while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) {
            tok_end--;
        }
        size_t len = (size_t) (tok_end - tok);
        for (const char **sp = server_protocols; *sp; sp++) {
            size_t sp_len = strlen(*sp);
            if (sp_len == len && memcmp(tok, *sp, len) == 0) {
                return xr_strndup(tok, len);
            }
        }
        while (*p && *p != ',')
            p++;
    }
    return NULL;
}

XrWebSocket *xr_ws_upgrade_ex(struct XrayIsolate *isolate, int fd, const char *request_headers,
                              const XrWsUpgradeOptions *opts) {
    if (fd < 0 || !request_headers)
        return NULL;

    // Check if upgrade request
    if (!xr_ws_is_upgrade_request(request_headers))
        return NULL;

    // Origin allowlist check (W-18). Cross-origin browser attacks are the
    // main threat model here: a malicious page can open wss://victim.tld
    // and replay the user's cookies. Callers that want to keep the legacy
    // "allow all" behaviour leave opts->allowed_origins NULL.
    if (opts && opts->allowed_origins) {
        char origin_val[256];
        const char *origin = NULL;
        if (ws_get_header_value(request_headers, "Origin", origin_val, sizeof(origin_val))) {
            origin = origin_val;
        }
        if (!ws_origin_allowed(origin, opts->allowed_origins)) {
            ws_send_simple_response(fd, 403, "Forbidden");
            return NULL;
        }
    }

    // Get Sec-WebSocket-Key
    char *sec_key = xr_ws_get_sec_key(request_headers);
    if (!sec_key)
        return NULL;

    // Check if client offered permessage-deflate — walk the extensions
    // header token list instead of doing a substring scan over the whole
    // request blob (W-13).
    bool client_deflate = false;
    {
        char ext_val[512];
        if (ws_get_header_value(request_headers, "Sec-WebSocket-Extensions", ext_val,
                                sizeof(ext_val))) {
            client_deflate = ws_header_list_has_token(ext_val, "permessage-deflate");
        }
    }

    // Subprotocol negotiation (W-19).
    char *picked_proto = NULL;
    if (opts && opts->server_protocols) {
        picked_proto = xr_ws_pick_subprotocol(request_headers, opts->server_protocols);
    }

    // Send upgrade response (with deflate + picked subprotocol if any)
    if (xr_ws_send_upgrade_response(fd, sec_key, picked_proto, client_deflate) < 0) {
        xr_free(sec_key);
        xr_free(picked_proto);
        return NULL;
    }

    // Create WebSocket connection object
    XrWebSocket *ws = (XrWebSocket *) xr_calloc(1, sizeof(XrWebSocket));
    if (!ws) {
        xr_free(sec_key);
        xr_free(picked_proto);
        return NULL;
    }

    ws->fd = fd;
    ws->state = WS_STATE_OPEN;
    ws->sec_key = sec_key;
    ws->protocol = picked_proto;  // transferred ownership; freed by xr_ws_free
    ws->is_server = true;         // Server-side connection: no masking on send
    ws->isolate = isolate;        // Store isolate for coroutine-aware I/O
    ws->deflate_enabled = client_deflate;
    ws->deflate_no_context = true;

    // Socket should already be non-blocking from the accept, but ensure it
    // (Required for coroutine-aware I/O with netpoll)
    xr_socket_set_nonblocking(fd);

    // Disable Nagle's algorithm + low-latency write wakeup
    xr_socket_set_nodelay(fd, true);
#ifdef TCP_NOTSENT_LOWAT
    int lowat = 16384;
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, (const char *) &lowat, sizeof(lowat));
#endif
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *) &nosig, sizeof(nosig));
#endif

    // Initialize buffer state (rbuf lazy-allocated on first recv)
    ws->rbuf = NULL;
    ws->rbuf_off = 0;
    ws->rbuf_len = 0;
    ws->rbuf_cap = 0;
    ws->msg_buf = NULL;
    ws->msg_buf_size = 0;
    ws->msg_buf_len = 0;
    ws->msg_remaining = 0;
    ws->frame_in_progress = false;
    ws->last_ping_sent_ms = 0;
    ws->last_pong_recv_ms = ws_now_ms();
    ws->ping_in_flight = false;
    ws->cached_pd = NULL;
    ws->wbuf = NULL;
    ws->wbuf_len = 0;
    ws->wbuf_cap = 0;
    ws->corked = false;

    // Set default config
    xr_ws_config_init(&ws->config);

    return ws;
}

XrWebSocket *xr_ws_upgrade(struct XrayIsolate *isolate, int fd, const char *request_headers) {
    // Thin wrapper preserving the legacy "no Origin / no subprotocol"
    // behaviour; new callers should use xr_ws_upgrade_ex directly.
    return xr_ws_upgrade_ex(isolate, fd, request_headers, NULL);
}
