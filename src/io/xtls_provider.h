/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtls_provider.h - TLS/SSL provider interface
 *
 * KEY CONCEPT:
 *   OpenSSL-based TLS support providing client/server connections,
 *   certificate verification, SNI, and ALPN negotiation.
 */
#ifndef XR_IO_XTLS_PROVIDER_H
#define XR_IO_XTLS_PROVIDER_H

#include "../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

struct XrVMRuntime;

/* ========== TLS Context ========== */

typedef struct XrTlsContext XrTlsContext;
typedef struct XrTlsConn XrTlsConn;

// TLS error codes — aliases into unified XrNetError
#include "xnet_error.h"
typedef XrNetError XrTlsError;
#define XR_TLS_OK XR_NERR_OK
#define XR_TLS_ERR_INIT XR_NERR_TLS_INIT
#define XR_TLS_ERR_CERT XR_NERR_TLS_CERT
#define XR_TLS_ERR_HANDSHAKE XR_NERR_TLS_HANDSHAKE
#define XR_TLS_ERR_READ XR_NERR_READ
#define XR_TLS_ERR_WRITE XR_NERR_WRITE
#define XR_TLS_ERR_CLOSED XR_NERR_CLOSED
#define XR_TLS_ERR_VERIFY XR_NERR_TLS_VERIFY

/* ========== Availability ========== */

// Returns true if TLS support was compiled in (OpenSSL found at build time)
XR_FUNC bool xr_tls_is_available(void);

/* ========== Global Initialization ========== */

// Initialize the TLS library (process-level, call once)
XR_FUNC void xr_tls_init(void);

/* ========== TLS Context Management ========== */

// Create a client TLS context
XR_FUNC XrTlsContext *xr_tls_context_new_client(void);

// Create a server TLS context
XR_FUNC XrTlsContext *xr_tls_context_new_server(const char *cert_file, const char *key_file);

// Load a PEM certificate and its private key into an existing context.
XR_FUNC int xr_tls_context_load_identity(XrTlsContext *ctx, const char *cert_file,
                                         const char *key_file);

// Free a TLS context
XR_FUNC void xr_tls_context_free(XrTlsContext *ctx);

// Enable peer verification. On a server, true also requires the client to
// present a certificate.
XR_FUNC void xr_tls_context_set_verify(XrTlsContext *ctx, bool verify);

/*
 * Load a PEM CA bundle used to verify the peer's certificate chain.
 *
 * ca_file: path to a PEM file. If the path points to a directory, OpenSSL
 *          is asked to use it as CApath instead. Passing NULL resets the
 *          context to the system default verify paths.
 *
 * Returns 0 on success, -1 on error (invalid ctx, unreadable file, etc.).
 *
 * Typical use: per-cluster / per-service root of trust, e.g. a private CA
 * issued to the cluster nodes. Without this call the context relies on
 * SSL_CTX_set_default_verify_paths() (system trust store).
 */
XR_FUNC int xr_tls_context_load_ca(XrTlsContext *ctx, const char *ca_file);

/* ========== TLS Connection Management ========== */

// Create a TLS connection (wrap an already connected socket)
XR_FUNC XrTlsConn *xr_tls_conn_new(XrTlsContext *ctx, int fd);

// Free a TLS connection
XR_FUNC void xr_tls_conn_free(XrTlsConn *conn);

// Set the SNI hostname (client)
XR_FUNC int xr_tls_conn_set_hostname(XrTlsConn *conn, const char *hostname);

// Set the ALPN protocol list (client)
XR_FUNC int xr_tls_context_set_alpn(XrTlsContext *ctx, const unsigned char *protocols, size_t len);

// Borrow the negotiated ALPN protocol bytes from the TLS provider.
// The returned view remains valid until the connection is freed.
XR_FUNC bool xr_tls_conn_get_alpn(XrTlsConn *conn, const unsigned char **protocol, size_t *length);

// Set the ALPN callback (server)
typedef int (*XrAlpnSelectCallback)(const unsigned char **out, unsigned char *outlen,
                                    const unsigned char *in, unsigned int inlen, void *arg);

// Perform a TLS handshake (client) - yields the calling coroutine on
// SSL_ERROR_WANT_READ/WRITE via the supplied isolate. X may be NULL
// only on bootstrap / tooling paths that intentionally spin.
XR_FUNC XrTlsError xr_tls_conn_handshake_client(struct XrVMRuntime *X, XrTlsConn *conn);

// Non-blocking handshake try (single SSL_connect attempt)
// Returns: 0=done, 1=WANT_READ, 2=WANT_WRITE, -1=error
XR_FUNC int xr_tls_conn_handshake_try(XrTlsConn *conn);

// Non-blocking server handshake try (single SSL_accept attempt).
// Returns: 0=done, 1=WANT_READ, 2=WANT_WRITE, -1=error.
XR_FUNC int xr_tls_conn_handshake_server_try(XrTlsConn *conn);

// Read data (yields on SSL_ERROR_WANT_READ/WRITE via X).
XR_FUNC int xr_tls_conn_read(struct XrVMRuntime *X, XrTlsConn *conn, void *buf, size_t len);

// Non-blocking read try (single SSL_read attempt)
// Returns: >0=bytes, 0=EOF, -1=WANT_READ, -2=WANT_WRITE, -3=error
XR_FUNC int xr_tls_conn_read_try(XrTlsConn *conn, void *buf, size_t len);

// Write data (yields on SSL_ERROR_WANT_READ/WRITE via X).
XR_FUNC int xr_tls_conn_write(struct XrVMRuntime *X, XrTlsConn *conn, const void *buf, size_t len);

// Non-blocking write try (single SSL_write attempt)
// Returns: >0=bytes, -1=WANT_WRITE, -2=WANT_READ, -3=error
XR_FUNC int xr_tls_conn_write_try(XrTlsConn *conn, const void *buf, size_t len);

// Close the connection
XR_FUNC void xr_tls_conn_close(XrTlsConn *conn);

// Get the error description
XR_FUNC const char *xr_tls_error_string(XrTlsError err);

#endif  // XR_IO_XTLS_PROVIDER_H
