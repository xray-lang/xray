/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * tls.c - TLS/SSL support layer implementation
 *
 * KEY CONCEPT:
 *   OpenSSL-based TLS with coroutine-friendly I/O integration.
 *   Supports SNI, ALPN, certificate verification.
 */

#include "tls.h"

#ifdef XR_ENABLE_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <string.h>

/* ========== Internal Structures ========== */

struct XrTlsContext {
    SSL_CTX *ssl_ctx;
    bool is_client;
    bool verify_peer;
};

struct XrTlsConn {
    SSL *ssl;
    int fd;
    XrTlsContext *ctx;
};

/* ========== Global Initialization ========== */

static int tls_initialized = 0;

void xr_tls_init(void) {
    if (tls_initialized) return;

    // OpenSSL 1.1.0+ auto-initializes, but explicit call is safer
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    tls_initialized = 1;
}

void xr_tls_cleanup(void) {
    if (!tls_initialized) return;

    // OpenSSL 1.1.0+ auto-cleans
    tls_initialized = 0;
}

/* ========== TLS Context ========== */

XrTlsContext* xr_tls_context_new_client(void) {
    xr_tls_init();

    XrTlsContext *ctx = (XrTlsContext*)calloc(1, sizeof(XrTlsContext));
    if (!ctx) return NULL;

    ctx->is_client = true;
    ctx->verify_peer = true;  // Verify certificate by default

    // Create SSL context with TLS client method
    ctx->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    // Min TLS 1.2
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    // Load system CA certificates
    SSL_CTX_set_default_verify_paths(ctx->ssl_ctx);

    // Set verification mode
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);

    return ctx;
}

XrTlsContext* xr_tls_context_new_server(const char *cert_file, const char *key_file) {
    xr_tls_init();

    if (!cert_file || !key_file) return NULL;

    XrTlsContext *ctx = (XrTlsContext*)calloc(1, sizeof(XrTlsContext));
    if (!ctx) return NULL;

    ctx->is_client = false;

    // Create SSL context with TLS server method
    ctx->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    // Min TLS 1.2
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    // Load certificate
    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    // Load private key
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    // Verify private key matches certificate
    if (!SSL_CTX_check_private_key(ctx->ssl_ctx)) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void xr_tls_context_free(XrTlsContext *ctx) {
    if (!ctx) return;

    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    free(ctx);
}

void xr_tls_context_set_verify(XrTlsContext *ctx, bool verify) {
    if (!ctx || !ctx->ssl_ctx) return;

    ctx->verify_peer = verify;

    if (verify) {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
}

int xr_tls_context_load_ca(XrTlsContext *ctx, const char *ca_file) {
    if (!ctx || !ctx->ssl_ctx) return -1;

    // NULL resets to the system default trust store so callers can strip
    // a previously pinned CA without rebuilding the context.
    if (!ca_file || !ca_file[0]) {
        if (SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) != 1) {
            return -1;
        }
        return 0;
    }

    // Heuristically distinguish "file" vs "directory" inputs. OpenSSL's
    // SSL_CTX_load_verify_locations accepts either; we pick based on a
    // trailing slash to avoid a stat() round-trip and to keep the error
    // path simple — passing both paths is also valid.
    size_t len = strlen(ca_file);
    const char *path_file = NULL;
    const char *path_dir = NULL;
    if (len > 0 && (ca_file[len - 1] == '/' || ca_file[len - 1] == '\\')) {
        path_dir = ca_file;
    } else {
        path_file = ca_file;
    }
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, path_file, path_dir) != 1) {
        return -1;
    }
    return 0;
}

/* ========== TLS Connection ========== */

XrTlsConn* xr_tls_conn_new(XrTlsContext *ctx, int fd) {
    if (!ctx || fd < 0) return NULL;

    XrTlsConn *conn = (XrTlsConn*)calloc(1, sizeof(XrTlsConn));
    if (!conn) return NULL;

    conn->ctx = ctx;
    conn->fd = fd;

    // Create SSL object
    conn->ssl = SSL_new(ctx->ssl_ctx);
    if (!conn->ssl) {
        free(conn);
        return NULL;
    }

    // Bind socket
    if (SSL_set_fd(conn->ssl, fd) != 1) {
        SSL_free(conn->ssl);
        free(conn);
        return NULL;
    }

    return conn;
}

void xr_tls_conn_free(XrTlsConn *conn) {
    if (!conn) return;

    if (conn->ssl) {
        SSL_free(conn->ssl);
    }
    free(conn);
}

int xr_tls_conn_set_hostname(XrTlsConn *conn, const char *hostname) {
    if (!conn || !conn->ssl || !hostname) return -1;

    // Set SNI
    if (SSL_set_tlsext_host_name(conn->ssl, hostname) != 1) {
        return -1;
    }

    // Set hostname verification
    SSL_set_hostflags(conn->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(conn->ssl, hostname) != 1) {
        return -1;
    }

    return 0;
}

int xr_tls_context_set_alpn(XrTlsContext *ctx, const unsigned char *protocols, size_t len) {
    if (!ctx || !ctx->ssl_ctx || !protocols || len == 0) return -1;

    // Set client ALPN protocol list
    if (SSL_CTX_set_alpn_protos(ctx->ssl_ctx, protocols, (unsigned int)len) != 0) {
        return -1;
    }
    return 0;
}

const char* xr_tls_conn_get_alpn(XrTlsConn *conn) {
    if (!conn || !conn->ssl) return NULL;

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;

    SSL_get0_alpn_selected(conn->ssl, &alpn, &alpn_len);

    if (!alpn || alpn_len == 0) return NULL;

    static _Thread_local char alpn_str[32];
    if (alpn_len >= sizeof(alpn_str)) alpn_len = sizeof(alpn_str) - 1;
    memcpy(alpn_str, alpn, alpn_len);
    alpn_str[alpn_len] = '\0';

    return alpn_str;
}

// ALPN server selection callback
static int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg) {
    (void)ssl;
    XrAlpnSelectCallback cb = (XrAlpnSelectCallback)arg;
    if (cb) {
        return cb(out, outlen, in, inlen, NULL);
    }

    // Default: select h2 or http/1.1
    const unsigned char *p = in;
    const unsigned char *end = in + inlen;

    while (p < end) {
        unsigned char len = *p++;
        if (p + len > end) break;

        // Prefer h2
        if (len == 2 && memcmp(p, "h2", 2) == 0) {
            *out = p;
            *outlen = len;
            return SSL_TLSEXT_ERR_OK;
        }
        p += len;
    }

    // Fallback to http/1.1
    p = in;
    while (p < end) {
        unsigned char len = *p++;
        if (p + len > end) break;

        if (len == 8 && memcmp(p, "http/1.1", 8) == 0) {
            *out = p;
            *outlen = len;
            return SSL_TLSEXT_ERR_OK;
        }
        p += len;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

void xr_tls_context_set_alpn_callback(XrTlsContext *ctx, XrAlpnSelectCallback cb, void *arg) {
    if (!ctx || !ctx->ssl_ctx) return;
    (void)arg;

    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, alpn_select_cb, (void*)cb);
}

// External: coroutine-safe socket API
extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern int xr_socket_write(struct XrayIsolate *X, int fd, const char *buf, size_t len);

// Thread-local: current VM instance
extern struct XrayIsolate* xr_io_get_isolate(void);

XrTlsError xr_tls_conn_handshake_client(XrTlsConn *conn) {
    if (!conn || !conn->ssl) return XR_TLS_ERR_INIT;

    while (1) {
        int ret = SSL_connect(conn->ssl);

        if (ret == 1) {
            return XR_TLS_OK;
        }

        int err = SSL_get_error(conn->ssl, ret);

        switch (err) {
            case SSL_ERROR_WANT_READ:
                // Wait for readable
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        char tmp[1];
                        int wait_ret = xr_socket_read(X, conn->fd, tmp, 0);
                        if (wait_ret == -2) {
                            // Coroutine yielded
                            return (XrTlsError)-2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_WANT_WRITE:
                // Wait for writable
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        int wait_ret = xr_socket_write(X, conn->fd, NULL, 0);
                        if (wait_ret == -2) {
                            return (XrTlsError)-2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_SSL:
                if (SSL_get_verify_result(conn->ssl) != X509_V_OK) {
                    return XR_TLS_ERR_VERIFY;
                }
                return XR_TLS_ERR_HANDSHAKE;

            case SSL_ERROR_SYSCALL:
                return XR_TLS_ERR_HANDSHAKE;

            default:
                return XR_TLS_ERR_HANDSHAKE;
        }
    }
}

XrTlsError xr_tls_conn_handshake_server(XrTlsConn *conn) {
    if (!conn || !conn->ssl) return XR_TLS_ERR_INIT;

    while (1) {
        int ret = SSL_accept(conn->ssl);

        if (ret == 1) {
            return XR_TLS_OK;
        }

        int err = SSL_get_error(conn->ssl, ret);

        switch (err) {
            case SSL_ERROR_WANT_READ:
                // Wait for readable
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        char tmp[1];
                        int wait_ret = xr_socket_read(X, conn->fd, tmp, 0);
                        if (wait_ret == -2) {
                            return (XrTlsError)-2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_WANT_WRITE:
                // Wait for writable
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        int wait_ret = xr_socket_write(X, conn->fd, NULL, 0);
                        if (wait_ret == -2) {
                            return (XrTlsError)-2;
                        }
                    }
                    continue;
                }

            default:
                return XR_TLS_ERR_HANDSHAKE;
        }
    }
}

int xr_tls_conn_read(XrTlsConn *conn, void *buf, size_t len) {
    if (!conn || !conn->ssl) return -1;

    while (1) {
        int ret = SSL_read(conn->ssl, buf, (int)len);

        if (ret > 0) {
            return ret;
        }

        int err = SSL_get_error(conn->ssl, ret);

        switch (err) {
            case SSL_ERROR_ZERO_RETURN:
                // Peer closed
                return 0;

            case SSL_ERROR_WANT_READ:
                // Wait for readable (coroutine-safe)
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        char tmp[1];
                        int wait_ret = xr_socket_read(X, conn->fd, tmp, 0);
                        if (wait_ret == -2) {
                            // Coroutine yielded
                            return -2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_WANT_WRITE:
                // Wait for writable (renegotiation)
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        int wait_ret = xr_socket_write(X, conn->fd, NULL, 0);
                        if (wait_ret == -2) {
                            return -2;
                        }
                    }
                    continue;
                }

            default:
                // Other error
                return -1;
        }
    }
}

int xr_tls_conn_write(XrTlsConn *conn, const void *buf, size_t len) {
    if (!conn || !conn->ssl) return -1;

    while (1) {
        int ret = SSL_write(conn->ssl, buf, (int)len);

        if (ret > 0) {
            return ret;
        }

        int err = SSL_get_error(conn->ssl, ret);

        switch (err) {
            case SSL_ERROR_WANT_WRITE:
                // Wait for writable
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        int wait_ret = xr_socket_write(X, conn->fd, NULL, 0);
                        if (wait_ret == -2) {
                            return -2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_WANT_READ:
                // Wait for readable (renegotiation)
                {
                    struct XrayIsolate *X = xr_io_get_isolate();
                    if (X) {
                        char tmp[1];
                        int wait_ret = xr_socket_read(X, conn->fd, tmp, 0);
                        if (wait_ret == -2) {
                            return -2;
                        }
                    }
                    continue;
                }

            case SSL_ERROR_ZERO_RETURN:
                // Connection closed
                return 0;

            default:
                // Other error
                return -1;
        }
    }
}

// ========== Non-blocking Try API ==========

int xr_tls_conn_handshake_try(XrTlsConn *conn) {
    if (!conn || !conn->ssl) return -1;

    int ret = SSL_connect(conn->ssl);
    if (ret == 1) return 0; // success

    int err = SSL_get_error(conn->ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:  return 1;
        case SSL_ERROR_WANT_WRITE: return 2;
        default:                   return -1;
    }
}

int xr_tls_conn_read_try(XrTlsConn *conn, void *buf, size_t len) {
    if (!conn || !conn->ssl) return -3;

    int ret = SSL_read(conn->ssl, buf, (int)len);
    if (ret > 0) return ret;
    if (ret == 0) return 0; // EOF

    int err = SSL_get_error(conn->ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:    return -1;
        case SSL_ERROR_WANT_WRITE:   return -2;
        case SSL_ERROR_ZERO_RETURN:  return 0;
        default:                     return -3;
    }
}

int xr_tls_conn_write_try(XrTlsConn *conn, const void *buf, size_t len) {
    if (!conn || !conn->ssl) return -3;

    int ret = SSL_write(conn->ssl, buf, (int)len);
    if (ret > 0) return ret;

    int err = SSL_get_error(conn->ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_WRITE:   return -1;
        case SSL_ERROR_WANT_READ:    return -2;
        case SSL_ERROR_ZERO_RETURN:  return 0;
        default:                     return -3;
    }
}

void xr_tls_conn_close(XrTlsConn *conn) {
    if (!conn || !conn->ssl) return;

    // Send close_notify
    SSL_shutdown(conn->ssl);
}

int xr_tls_conn_get_fd(XrTlsConn *conn) {
    if (!conn) return -1;
    return conn->fd;
}

const char* xr_tls_error_string(XrTlsError err) {
    switch (err) {
        case XR_TLS_OK: return "Success";
        case XR_TLS_ERR_INIT: return "TLS initialization failed";
        case XR_TLS_ERR_CERT: return "Certificate error";
        case XR_TLS_ERR_HANDSHAKE: return "TLS handshake failed";
        case XR_TLS_ERR_READ: return "TLS read failed";
        case XR_TLS_ERR_WRITE: return "TLS write failed";
        case XR_TLS_ERR_CLOSED: return "Connection closed";
        case XR_TLS_ERR_VERIFY: return "Certificate verification failed";
        default: return "Unknown TLS error";
    }
}

#else // !XR_ENABLE_TLS

// Empty implementations when TLS is disabled

void xr_tls_init(void) { }
void xr_tls_cleanup(void) {}
XrTlsContext* xr_tls_context_new_client(void) { return NULL; }
XrTlsContext* xr_tls_context_new_server(const char *cert_file, const char *key_file) { (void)cert_file; (void)key_file; return NULL; }
void xr_tls_context_free(XrTlsContext *ctx) { (void)ctx; }
void xr_tls_context_set_verify(XrTlsContext *ctx, bool verify) { (void)ctx; (void)verify; }
int xr_tls_context_load_ca(XrTlsContext *ctx, const char *ca_file) { (void)ctx; (void)ca_file; return -1; }
int xr_tls_context_set_alpn(XrTlsContext *ctx, const unsigned char *protocols, size_t len) { (void)ctx; (void)protocols; (void)len; return -1; }
void xr_tls_context_set_alpn_callback(XrTlsContext *ctx, XrAlpnSelectCallback cb, void *arg) { (void)ctx; (void)cb; (void)arg; }
XrTlsConn* xr_tls_conn_new(XrTlsContext *ctx, int fd) { (void)ctx; (void)fd; return NULL; }
void xr_tls_conn_free(XrTlsConn *conn) { (void)conn; }
int xr_tls_conn_set_hostname(XrTlsConn *conn, const char *hostname) { (void)conn; (void)hostname; return -1; }
const char* xr_tls_conn_get_alpn(XrTlsConn *conn) { (void)conn; return NULL; }
XrTlsError xr_tls_conn_handshake_client(XrTlsConn *conn) { (void)conn; return XR_TLS_ERR_INIT; }
XrTlsError xr_tls_conn_handshake_server(XrTlsConn *conn) { (void)conn; return XR_TLS_ERR_INIT; }
int xr_tls_conn_handshake_try(XrTlsConn *conn) { (void)conn; return -1; }
int xr_tls_conn_read(XrTlsConn *conn, void *buf, size_t len) { (void)conn; (void)buf; (void)len; return -1; }
int xr_tls_conn_read_try(XrTlsConn *conn, void *buf, size_t len) { (void)conn; (void)buf; (void)len; return -3; }
int xr_tls_conn_write(XrTlsConn *conn, const void *buf, size_t len) { (void)conn; (void)buf; (void)len; return -1; }
int xr_tls_conn_write_try(XrTlsConn *conn, const void *buf, size_t len) { (void)conn; (void)buf; (void)len; return -3; }
void xr_tls_conn_close(XrTlsConn *conn) { (void)conn; }
int xr_tls_conn_get_fd(XrTlsConn *conn) { (void)conn; return -1; }
const char* xr_tls_error_string(XrTlsError err) { (void)err; return "TLS not enabled"; }

#endif
