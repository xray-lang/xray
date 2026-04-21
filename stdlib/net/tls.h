/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * tls.h - TLS/SSL support layer
 *
 * KEY CONCEPT:
 *   OpenSSL-based TLS support providing client/server connections,
 *   certificate verification, SNI, and ALPN negotiation.
 */
#ifndef XR_STDLIB_TLS_H
#define XR_STDLIB_TLS_H

#include "../../src/base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

/* ========== TLS Context ========== */

typedef struct XrTlsContext XrTlsContext;
typedef struct XrTlsConn XrTlsConn;

// TLS error codes — aliases into unified XrNetError
#include "xneterror.h"
typedef XrNetError XrTlsError;
#define XR_TLS_OK            XR_NERR_OK
#define XR_TLS_ERR_INIT      XR_NERR_TLS_INIT
#define XR_TLS_ERR_CERT      XR_NERR_TLS_CERT
#define XR_TLS_ERR_HANDSHAKE XR_NERR_TLS_HANDSHAKE
#define XR_TLS_ERR_READ      XR_NERR_READ
#define XR_TLS_ERR_WRITE     XR_NERR_WRITE
#define XR_TLS_ERR_CLOSED    XR_NERR_CLOSED
#define XR_TLS_ERR_VERIFY    XR_NERR_TLS_VERIFY

/* ========== Global Initialization ========== */

// Initialize the TLS library (process-level, call once)
XR_FUNC void xr_tls_init(void);

// Clean up the TLS library
XR_FUNC void xr_tls_cleanup(void);

/* ========== TLS Context Management ========== */

// Create a client TLS context
XR_FUNC XrTlsContext* xr_tls_context_new_client(void);

// Create a server TLS context
XR_FUNC XrTlsContext* xr_tls_context_new_server(const char *cert_file, const char *key_file);

// Free a TLS context
XR_FUNC void xr_tls_context_free(XrTlsContext *ctx);

// Set whether to verify certificates (client)
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
XR_FUNC XrTlsConn* xr_tls_conn_new(XrTlsContext *ctx, int fd);

// Free a TLS connection
XR_FUNC void xr_tls_conn_free(XrTlsConn *conn);

// Set the SNI hostname (client)
XR_FUNC int xr_tls_conn_set_hostname(XrTlsConn *conn, const char *hostname);

// Set the ALPN protocol list (client)
XR_FUNC int xr_tls_context_set_alpn(XrTlsContext *ctx, const unsigned char *protocols, size_t len);

// Get the negotiated ALPN protocol
XR_FUNC const char* xr_tls_conn_get_alpn(XrTlsConn *conn);

// Set the ALPN callback (server)
typedef int (*XrAlpnSelectCallback)(const unsigned char **out, unsigned char *outlen,
                                     const unsigned char *in, unsigned int inlen,
                                     void *arg);
XR_FUNC void xr_tls_context_set_alpn_callback(XrTlsContext *ctx, XrAlpnSelectCallback cb, void *arg);

// Perform a TLS handshake (client) - blocking, uses xr_socket_read/write
XR_FUNC XrTlsError xr_tls_conn_handshake_client(XrTlsConn *conn);

// Perform a TLS handshake (server) - blocking, uses xr_socket_read/write
XR_FUNC XrTlsError xr_tls_conn_handshake_server(XrTlsConn *conn);

// Non-blocking handshake try (single SSL_connect attempt)
// Returns: 0=done, 1=WANT_READ, 2=WANT_WRITE, -1=error
XR_FUNC int xr_tls_conn_handshake_try(XrTlsConn *conn);

// Read data (blocking, uses xr_socket_read for waiting)
XR_FUNC int xr_tls_conn_read(XrTlsConn *conn, void *buf, size_t len);

// Non-blocking read try (single SSL_read attempt)
// Returns: >0=bytes, 0=EOF, -1=WANT_READ, -2=WANT_WRITE, -3=error
XR_FUNC int xr_tls_conn_read_try(XrTlsConn *conn, void *buf, size_t len);

// Write data (blocking, uses xr_socket_write for waiting)
XR_FUNC int xr_tls_conn_write(XrTlsConn *conn, const void *buf, size_t len);

// Non-blocking write try (single SSL_write attempt)
// Returns: >0=bytes, -1=WANT_WRITE, -2=WANT_READ, -3=error
XR_FUNC int xr_tls_conn_write_try(XrTlsConn *conn, const void *buf, size_t len);

// Close the connection
XR_FUNC void xr_tls_conn_close(XrTlsConn *conn);

// Get the underlying socket fd
XR_FUNC int xr_tls_conn_get_fd(XrTlsConn *conn);

// Get the error description
XR_FUNC const char* xr_tls_error_string(XrTlsError err);

/* ========== Production Features (P17) ========== */

/*
 * Load client certificate + private key for mutual TLS (mTLS).
 * Both files must be PEM-encoded.
 * Returns 0 on success, -1 on error.
 */
XR_FUNC int xr_tls_context_set_client_cert(XrTlsContext *ctx,
                                             const char *cert_file,
                                             const char *key_file);

/*
 * Enable TLS session caching on the context.
 * Client contexts cache sessions so repeated connections to the same
 * server can resume with an abbreviated handshake.
 * Returns 0 on success, -1 on error.
 */
XR_FUNC int xr_tls_context_enable_session_cache(XrTlsContext *ctx);

/*
 * Retrieve the current session from a connected TLS connection.
 * Returns an opaque pointer (SSL_SESSION*) the caller can pass to
 * xr_tls_conn_set_session on a new connection to attempt resumption.
 * The returned session must be freed with xr_tls_session_free().
 * Returns NULL if no session is available.
 */
XR_FUNC void* xr_tls_conn_get_session(XrTlsConn *conn);

/*
 * Set a previously saved session for resumption.
 * Must be called before the handshake. The session is not consumed;
 * the caller still owns and must free it.
 * Returns 0 on success, -1 on error.
 */
XR_FUNC int xr_tls_conn_set_session(XrTlsConn *conn, void *session);

/*
 * Free a session obtained from xr_tls_conn_get_session.
 */
XR_FUNC void xr_tls_session_free(void *session);

/*
 * Check whether the current connection was resumed from a cached session.
 * Returns true if the handshake was an abbreviated (resumed) handshake.
 */
XR_FUNC bool xr_tls_conn_is_resumed(XrTlsConn *conn);

/*
 * Request OCSP stapling from the server (client-side).
 * Must be called on a client context before creating connections.
 * When enabled, the client will request an OCSP response via the
 * TLS status_request extension and verify it during the handshake.
 * Returns 0 on success, -1 on error.
 */
XR_FUNC int xr_tls_context_enable_ocsp_stapling(XrTlsContext *ctx);

#endif // XR_STDLIB_TLS_H
