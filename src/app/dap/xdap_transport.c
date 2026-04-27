/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_transport.c - DAP transport layer implementation
 */

#include "xdap_transport.h"
#include "../../base/xframing.h"
#include "../../base/xfd.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#define ssize_t int
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#endif

#define INITIAL_BUF_SIZE 4096
#define MAX_HEADER_SIZE 256

// Outgoing queue cap. DAP events can burst (e.g. a busy output stream
// or a deep variable tree) while the IDE briefly lags on reads. We
// allow up to 16 MiB of queued bytes before declaring the peer dead —
// large enough to absorb any realistic spike, small enough that a
// runaway producer still gets reined in long before we OOM the server.
#define XDAP_WRITE_BUF_MAX (16u * 1024u * 1024u)

// ============================================================================
// Internal Helpers
// ============================================================================

// Set fd to non-blocking mode
static int set_nonblock(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket((SOCKET) fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Read from fd into buffer (non-blocking)
// Returns: bytes read, 0 = would block, -1 = error/closed
static ssize_t read_nonblock(int fd, char *buf, size_t len) {
#ifdef _WIN32
    int n = recv((SOCKET) fd, buf, (int) len, 0);
    if (n < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
#else
    ssize_t n = read(fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
#endif
}

// Non-blocking single-shot write.
// Returns:
//   > 0: bytes actually written (may be 0 < ret < len on partial write)
//   0: would block — kernel buffer full, caller should queue & retry
//   -1: fatal error / peer closed.
static ssize_t write_nonblock(int fd, const char *buf, size_t len) {
#ifdef _WIN32
    int n = send((SOCKET) fd, buf, (int) len, 0);
    if (n < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
#else
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n == 0)
        return -1;
    return n;
#endif
}

// Ensure buffer has enough capacity
static bool ensure_capacity(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed)
        return true;
    size_t new_cap = *cap * 2;
    if (new_cap < needed)
        new_cap = needed;
    char *new_buf = xr_realloc(*buf, new_cap);
    if (!new_buf)
        return false;
    *buf = new_buf;
    *cap = new_cap;
    return true;
}

// ============================================================================
// Transport Creation
// ============================================================================

static XdapTransport *transport_alloc(XdapTransportType type) {
    XdapTransport *t = xr_calloc(1, sizeof(XdapTransport));
    if (!t)
        return NULL;

    t->type = type;
    t->read_fd = -1;
    t->write_fd = -1;
    t->listen_fd = -1;
    t->pending_content_length = -1;

    // Allocate buffers
    t->read_buf = xr_malloc(INITIAL_BUF_SIZE);
    t->read_cap = INITIAL_BUF_SIZE;
    t->write_buf = xr_malloc(INITIAL_BUF_SIZE);
    t->write_cap = INITIAL_BUF_SIZE;

    if (!t->read_buf || !t->write_buf) {
        xr_free(t->read_buf);
        xr_free(t->write_buf);
        xr_free(t);
        return NULL;
    }

    return t;
}

XdapTransport *xdap_transport_stdio(void) {
    XdapTransport *t = transport_alloc(XDAP_TRANSPORT_STDIO);
    if (!t)
        return NULL;

    t->read_fd = xr_stdin_fd();
    t->write_fd = xr_stdout_fd();

    // Set stdin to non-blocking
    set_nonblock(t->read_fd);

    // Disable stdout buffering
    setvbuf(stdout, NULL, _IONBF, 0);

    // Initialize poll
    if (xr_poll_init(&t->poll) < 0) {
        fprintf(stderr, "[DAP] Failed to initialize poll\n");
        xdap_transport_free(t);
        return NULL;
    }
    t->poll_initialized = true;

    // Add stdin to poll
    if (xr_poll_add(&t->poll, t->read_fd, XR_POLL_IN, t) < 0) {
        fprintf(stderr, "[DAP] Failed to add stdin to poll\n");
        xdap_transport_free(t);
        return NULL;
    }

    t->connected = true;
    return t;
}

XdapTransport *xdap_transport_tcp_server(int port) {
    XdapTransport *t = transport_alloc(XDAP_TRANSPORT_TCP_SERVER);
    if (!t)
        return NULL;

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        xdap_transport_free(t);
        return NULL;
    }
#endif

    // Create server socket
    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0) {
        fprintf(stderr, "[DAP] Failed to create socket: %s\n", strerror(errno));
        xdap_transport_free(t);
        return NULL;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &opt, sizeof(opt));
#endif

    // Bind to port
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t) port);

    if (bind(t->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[DAP] Failed to bind port %d: %s\n", port, strerror(errno));
        xdap_transport_free(t);
        return NULL;
    }

    // Get actual port
    socklen_t addr_len = sizeof(addr);
    getsockname(t->listen_fd, (struct sockaddr *) &addr, &addr_len);
    t->listen_port = ntohs(addr.sin_port);

    // Listen
    if (listen(t->listen_fd, 1) < 0) {
        fprintf(stderr, "[DAP] Failed to listen: %s\n", strerror(errno));
        xdap_transport_free(t);
        return NULL;
    }

    fprintf(stderr, "[DAP] Listening on port %d, waiting for debugger...\n", t->listen_port);

    // Non-blocking accept: poll the listen socket so we don't hang forever
    set_nonblock(t->listen_fd);

    // Temporary poll for accept (the main poll is initialized later for the client fd)
    XrPoll accept_poll;
    if (xr_poll_init(&accept_poll) < 0) {
        fprintf(stderr, "[DAP] Failed to initialize accept poll\n");
        xdap_transport_free(t);
        return NULL;
    }
    if (xr_poll_add(&accept_poll, t->listen_fd, XR_POLL_IN, NULL) < 0) {
        xr_poll_destroy(&accept_poll);
        xdap_transport_free(t);
        return NULL;
    }

    int client_fd = -1;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Poll with 500ms timeout so the process stays interruptible
    for (;;) {
        XrPollEvent ev[1];
        int n = xr_poll_wait(&accept_poll, ev, 1, 500);
        if (n < 0) {
            if (errno == EINTR)
                continue;  // Signal, retry
            fprintf(stderr, "[DAP] Accept poll error: %s\n", strerror(errno));
            break;
        }
        if (n > 0) {
            client_fd = accept(t->listen_fd, (struct sockaddr *) &client_addr, &client_len);
            if (client_fd >= 0)
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            fprintf(stderr, "[DAP] Failed to accept connection: %s\n", strerror(errno));
            break;
        }
        // n == 0: timeout, loop again
    }
    xr_poll_destroy(&accept_poll);

    if (client_fd < 0) {
        xdap_transport_free(t);
        return NULL;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    fprintf(stderr, "[DAP] Debugger connected from %s:%d\n", client_ip,
            ntohs(client_addr.sin_port));

    t->read_fd = client_fd;
    t->write_fd = client_fd;

    // Disable Nagle's algorithm for low-latency DAP messages
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof(nodelay));

    // Set client socket to non-blocking
    set_nonblock(t->read_fd);

    // Initialize poll
    if (xr_poll_init(&t->poll) < 0) {
        fprintf(stderr, "[DAP] Failed to initialize poll\n");
        xdap_transport_free(t);
        return NULL;
    }
    t->poll_initialized = true;

    // Add client socket to poll
    if (xr_poll_add(&t->poll, t->read_fd, XR_POLL_IN, t) < 0) {
        fprintf(stderr, "[DAP] Failed to add socket to poll\n");
        xdap_transport_free(t);
        return NULL;
    }

    t->connected = true;
    return t;
}

XdapTransport *xdap_transport_tcp_connect(const char *host, int port) {
    XdapTransport *t = transport_alloc(XDAP_TRANSPORT_TCP_CLIENT);
    if (!t)
        return NULL;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        xdap_transport_free(t);
        return NULL;
    }
#endif

    // Resolve host using getaddrinfo (thread-safe)
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo *result = NULL;
    int gai_err = getaddrinfo(host, port_str, &hints, &result);
    if (gai_err != 0) {
        fprintf(stderr, "[DAP] Failed to resolve host %s: %s\n", host, gai_strerror(gai_err));
        xdap_transport_free(t);
        return NULL;
    }

    // Create socket and connect
    int sock_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock_fd < 0) {
        fprintf(stderr, "[DAP] Failed to create socket: %s\n", strerror(errno));
        freeaddrinfo(result);
        xdap_transport_free(t);
        return NULL;
    }

    fprintf(stderr, "[DAP] Connecting to %s:%d...\n", host, port);

    if (connect(sock_fd, result->ai_addr, result->ai_addrlen) < 0) {
        fprintf(stderr, "[DAP] Failed to connect: %s\n", strerror(errno));
        close(sock_fd);
        freeaddrinfo(result);
        xdap_transport_free(t);
        return NULL;
    }
    freeaddrinfo(result);

    fprintf(stderr, "[DAP] Connected to %s:%d\n", host, port);

    t->read_fd = sock_fd;
    t->write_fd = sock_fd;

    // Disable Nagle's algorithm for low-latency DAP messages
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof(nodelay));

    // Set to non-blocking
    set_nonblock(t->read_fd);

    // Initialize poll
    if (xr_poll_init(&t->poll) < 0) {
        fprintf(stderr, "[DAP] Failed to initialize poll\n");
        xdap_transport_free(t);
        return NULL;
    }
    t->poll_initialized = true;

    if (xr_poll_add(&t->poll, t->read_fd, XR_POLL_IN, t) < 0) {
        fprintf(stderr, "[DAP] Failed to add socket to poll\n");
        xdap_transport_free(t);
        return NULL;
    }

    t->connected = true;
    return t;
}

void xdap_transport_free(XdapTransport *t) {
    if (!t)
        return;

    if (t->poll_initialized) {
        xr_poll_destroy(&t->poll);
    }

    if (t->type == XDAP_TRANSPORT_TCP_SERVER || t->type == XDAP_TRANSPORT_TCP_CLIENT) {
        if (t->read_fd >= 0)
            close(t->read_fd);
        // For TCP, write_fd == read_fd; for safety, only close if different
        if (t->write_fd >= 0 && t->write_fd != t->read_fd)
            close(t->write_fd);
        if (t->listen_fd >= 0)
            close(t->listen_fd);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    xr_free(t->read_buf);
    xr_free(t->write_buf);
    xr_free(t);
}

// ============================================================================
// Non-blocking I/O
// ============================================================================

int xdap_transport_poll(XdapTransport *t, int timeout_ms) {
    if (!t || !t->poll_initialized)
        return -1;

    XrPollEvent events[4];
    return xr_poll_wait(&t->poll, events, 4, timeout_ms);
}

// Forward decl — drain lives below try_read.
static bool try_drain_write(XdapTransport *t);

char *xdap_transport_try_read(XdapTransport *t, size_t *out_len, bool *would_block) {
    if (!t || !t->connected) {
        if (would_block)
            *would_block = false;
        return NULL;
    }

    // Opportunistic write drain on every read tick. Keeps queued
    // outbound bytes flowing even when the DAP protocol layer is
    // quiet, and piggybacks on poll events the main loop already
    // delivers for stdin — so there's no extra timer needed.
    if (!try_drain_write(t)) {
        if (would_block)
            *would_block = false;
        return NULL;
    }

    // Try to read more data into buffer
    if (!ensure_capacity(&t->read_buf, &t->read_cap, t->read_len + 1024)) {
        // OOM - treat as error
        if (would_block)
            *would_block = false;
        return NULL;
    }

    ssize_t n = read_nonblock(t->read_fd, t->read_buf + t->read_len, t->read_cap - t->read_len - 1);
    if (n > 0) {
        t->read_len += (size_t) n;
        t->read_buf[t->read_len] = '\0';  // Null-terminate for string search
    } else if (n < 0) {
        // Connection closed or error
        t->connected = false;
        if (would_block)
            *would_block = false;
        return NULL;
    }

    // Try to parse header via shared framing module
    if (t->pending_content_length < 0) {
        XrFrameStatus fs =
            xr_frame_parse(t->read_buf, t->read_len, &t->header_end, &t->pending_content_length);
        if (fs == XR_FRAME_ERROR) {
            t->connected = false;
            if (would_block)
                *would_block = false;
            return NULL;
        }
    }

    // Check if we have a complete message
    if (t->pending_content_length >= 0) {
        size_t total_needed = t->header_end + (size_t) t->pending_content_length;

        if (t->read_len >= total_needed) {
            // Extract message content
            int content_len = t->pending_content_length;
            char *content = xr_malloc((size_t) content_len + 1);
            if (!content) {
                if (would_block)
                    *would_block = false;
                return NULL;
            }

            memcpy(content, t->read_buf + t->header_end, (size_t) content_len);
            content[content_len] = '\0';

            // Remove consumed data from buffer
            size_t remaining = t->read_len - total_needed;
            if (remaining > 0) {
                memmove(t->read_buf, t->read_buf + total_needed, remaining);
            }
            t->read_len = remaining;
            t->read_buf[t->read_len] = '\0';

            // Reset header state for next message
            t->pending_content_length = -1;
            t->header_end = 0;

            if (out_len)
                *out_len = (size_t) content_len;
            if (would_block)
                *would_block = false;
            return content;
        }
    }

    // No complete message yet
    if (would_block)
        *would_block = true;
    return NULL;
}

// Try to flush as much of write_buf as the kernel will take without
// blocking. Advances write_len by memmove'ing unsent bytes to offset 0.
// Returns true if the write stream is still healthy (either fully
// drained, or only a partial drain because the kernel buffer is full);
// returns false on a fatal write error (peer closed, broken pipe).
static bool try_drain_write(XdapTransport *t) {
    if (!t || t->write_len == 0)
        return true;

    ssize_t sent = write_nonblock(t->write_fd, t->write_buf, t->write_len);
    if (sent < 0) {
        // Peer closed or fatal error — any queued bytes are lost but
        // that is what transport layers everywhere do when the pipe
        // dies; the DAP protocol layer will surface this to the UI
        // via the next disconnect event.
        t->connected = false;
        t->write_len = 0;
        return false;
    }
    if (sent == 0) {
        // Would-block: leave queue intact, caller retries later.
        return true;
    }
    size_t remaining = t->write_len - (size_t) sent;
    if (remaining > 0) {
        memmove(t->write_buf, t->write_buf + (size_t) sent, remaining);
    }
    t->write_len = remaining;
    return true;
}

// Queue one DAP message (header + body) into write_buf, then try to
// push as much as the kernel accepts right now without blocking.
// Any unsent tail remains in write_buf and will be drained by
// subsequent xdap_transport_try_read() calls (every main-loop tick
// visits try_read which in turn calls try_drain_write).
//
// If the cumulative queue exceeds XDAP_WRITE_BUF_MAX we mark the
// transport disconnected so the DAP session is torn down cleanly
// rather than accumulating unbounded memory against a dead peer.
void xdap_transport_write(XdapTransport *t, const char *json, size_t len) {
    if (!t || !t->connected)
        return;

    // Opportunistic drain first — often the queue is empty and this
    // call writes the whole message in a single syscall.
    if (!try_drain_write(t))
        return;

    char header[64];
    int header_len = xr_frame_write_header(header, sizeof(header), len);
    if (header_len < 0)
        return;

    size_t add = (size_t) header_len + len;
    size_t needed = t->write_len + add;

    if (needed > XDAP_WRITE_BUF_MAX) {
        // Back-pressure guard: the peer is not consuming fast enough.
        // Drop the connection instead of OOMing the debugger.
        fprintf(stderr, "[DAP] Write queue exceeds %u bytes; dropping connection\n",
                XDAP_WRITE_BUF_MAX);
        t->connected = false;
        t->write_len = 0;
        return;
    }

    if (!ensure_capacity(&t->write_buf, &t->write_cap, needed)) {
        // OOM on queue growth. Same treatment as a dead peer.
        t->connected = false;
        t->write_len = 0;
        return;
    }

    memcpy(t->write_buf + t->write_len, header, (size_t) header_len);
    t->write_len += (size_t) header_len;
    memcpy(t->write_buf + t->write_len, json, len);
    t->write_len += len;

    // Try once more after appending; the kernel may have freed up
    // space between the first drain and this point.
    try_drain_write(t);
}

void xdap_transport_wakeup(XdapTransport *t) {
    if (!t || !t->poll_initialized)
        return;
    xr_poll_wakeup(&t->poll);
}

// ============================================================================
// Status
// ============================================================================

bool xdap_transport_is_connected(XdapTransport *t) {
    return t && t->connected;
}

int xdap_transport_get_port(XdapTransport *t) {
    return t ? t->listen_port : 0;
}

int xdap_transport_get_fd(XdapTransport *t) {
    return t ? t->read_fd : -1;
}
