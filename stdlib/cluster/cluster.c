/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster.c - Cluster module top-level initialization and xray bindings
 *
 * KEY CONCEPT:
 *   Manages the cluster lifecycle (one per isolate) and provides xray-level
 *   function bindings (cluster.start, cluster.join, cluster.stop, etc.)
 */

#include "cluster.h"
#include "cluster_internal.h"
#include "../../stdlib/common.h"
#include "../crypto/crypto.h"  // xr_secure_wipe
#include "../../stdlib/net/io.h"
#include "../../stdlib/mem/mem.h"
#include "../../stdlib/stdlib_cache.h"
#include "../../src/runtime/class/xenum.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoro_registry.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xsocket.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/io/xdns.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/vm/xvm.h"
#include "../../src/base/xhash.h"
#include "../../src/base/xchecks.h"
#include "../../src/os/os_random.h"
#include "../../src/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* ========== Heartbeat Coroutine ========== */

/*
 * Drive cluster_health_send_heartbeats + cluster_health_check_heartbeats at a
 * steady cadence. Runs as a native coroutine on the normal worker
 * pool so the cluster stays on one scheduling model end-to-end — no
 * more stray pthread with its own sleep granularity.
 *
 * Ticks at heartbeat_interval_ms / 2 (capped at 500ms min) so that
 * phi-accrual has at least two samples per interval and stop-latency
 * is bounded.
 */
static void cluster_heartbeat_context_destroy(void *context) {
    XrCluster *c = (XrCluster *) context;
    if (!c)
        return;
    atomic_store(&c->heartbeat_running, false);
    cluster_runtime_release(c);
}

static XrCFuncResult cluster_heartbeat_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                                void *context, XrValue *result) {
    (void) resume_value;
    XrCluster *c = (XrCluster *) context;
    if (!c || !atomic_load(&c->running) || status == XR_RESUME_CANCELLED ||
        status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;

    cluster_health_send_heartbeats(c);
    cluster_health_check_heartbeats(c);

    int sleep_ms = c->heartbeat_interval_ms / 2;
    if (sleep_ms < 500)
        sleep_ms = 500;
    return xr_yield_for_timeout(X, sleep_ms, cluster_heartbeat_continue, c, result);
}

static XrCFuncResult cluster_heartbeat_entry(XrVMRuntime *X, void *context, XrValue *result) {
    XrCluster *c = (XrCluster *) context;
    if (!c || !atomic_load(&c->running))
        return XR_CFUNC_DONE;
    atomic_store(&c->heartbeat_running, true);
    int sleep_ms = c->heartbeat_interval_ms / 2;
    if (sleep_ms < 500)
        sleep_ms = 500;
    return xr_yield_for_timeout(X, sleep_ms, cluster_heartbeat_continue, c, result);
}

/* ========== Accept Loop ==========
 *
 * Runs as a native coroutine spawned from cluster_runtime_start. Handles
 * every inbound peer: coroutine-friendly accept, optional TLS wrap,
 * cluster handshake, then spawns writer+reader coroutines for the new
 * node. Terminates when the listen fd is closed by cluster_runtime_stop.
 *
 * The loop is intentionally forgiving of per-connection failures —
 * one bad peer (handshake timeout, wrong secret, expired cert) must
 * not take down the whole accept path. Only a fd-level failure
 * (EBADF from a closed listen_fd) exits the loop.
 */
typedef enum XrInboundPhase {
    XR_INBOUND_TLS,
    XR_INBOUND_READ_REQ_HEADER,
    XR_INBOUND_READ_REQ_PAYLOAD,
    XR_INBOUND_WRITE_ACK,
    XR_INBOUND_READ_DONE_HEADER,
    XR_INBOUND_READ_DONE_PAYLOAD,
} XrInboundPhase;

typedef struct XrInboundContext {
    XrCluster *cluster;
    XrIOConn *conn;
    XrInboundPhase phase;
    int64_t deadline_ms;
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    size_t header_used;
    uint8_t frame_type;
    uint8_t payload[512];
    uint32_t payload_len;
    uint32_t payload_used;
    uint8_t write_buf[512];
    size_t write_len;
    size_t write_used;
    XrFrameHandshakeReq request;
    XrFrameHandshakeAck ack;
} XrInboundContext;

static void cluster_inbound_context_destroy(void *context) {
    XrInboundContext *ctx = (XrInboundContext *) context;
    if (!ctx)
        return;
    if (ctx->conn)
        xr_io_close(ctx->conn);
    xr_secure_wipe(&ctx->request, sizeof(ctx->request));
    xr_secure_wipe(&ctx->ack, sizeof(ctx->ack));
    xr_secure_wipe(ctx->payload, sizeof(ctx->payload));
    xr_secure_wipe(ctx->write_buf, sizeof(ctx->write_buf));
    cluster_runtime_release(ctx->cluster);
    xr_free(ctx);
}

static XrCFuncResult cluster_inbound_drive(XrVMRuntime *X, XrInboundContext *ctx, XrValue *result);

static XrCFuncResult cluster_inbound_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                              void *context, XrValue *result) {
    (void) resume_value;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return cluster_inbound_drive(X, (XrInboundContext *) context, result);
}

static XrCFuncResult cluster_inbound_wait(XrVMRuntime *X, XrInboundContext *ctx, int events,
                                          XrValue *result) {
    int64_t remaining = ctx->deadline_ms - cluster_now_ms();
    if (remaining <= 0)
        return XR_CFUNC_DONE;
    return xr_yield_for_io(X, ctx->conn->fd, events, remaining, cluster_inbound_continue, ctx,
                           result);
}

static bool cluster_inbound_reset_read(XrInboundContext *ctx, XrInboundPhase phase) {
    ctx->phase = phase;
    ctx->header_used = 0;
    ctx->payload_len = 0;
    ctx->payload_used = 0;
    ctx->frame_type = 0;
    return true;
}

static XrCFuncResult cluster_inbound_complete(XrInboundContext *ctx) {
    XrFrameHandshakeDone done;
    if (cluster_frame_decode_handshake_done(ctx->payload, ctx->payload_len, &done) != 0)
        return XR_CFUNC_DONE;

    uint8_t expected_proof[XR_PROOF_SIZE];
    cluster_compute_proof(ctx->cluster->secret, ctx->ack.nonce, expected_proof);
    bool proof_ok = cluster_proof_equal(done.proof, expected_proof);
    xr_secure_wipe(expected_proof, sizeof(expected_proof));
    xr_secure_wipe(&done, sizeof(done));
    if (!proof_ok)
        return XR_CFUNC_DONE;

    XrClusterNode *node = cluster_node_new(ctx->request.name, NULL, 0);
    if (!node)
        return XR_CFUNC_DONE;
    node->conn = ctx->conn;
    node->state = XR_NODE_CONNECTED;
    node->flags = ctx->request.flags;
    node->last_heartbeat_recv = cluster_now_ms();
    ctx->conn = NULL;

    if (cluster_health_is_dead(ctx->cluster, node->name) || !cluster_node_add(ctx->cluster, node)) {
        cluster_node_shutdown(node);
        cluster_node_release(node);
        return XR_CFUNC_DONE;
    }
    if (!cluster_node_start_io(ctx->cluster, node) && cluster_node_remove(ctx->cluster, node)) {
        cluster_node_shutdown(node);
        cluster_node_release(node);
    }
    return XR_CFUNC_DONE;
}

static XrCFuncResult cluster_inbound_drive(XrVMRuntime *X, XrInboundContext *ctx, XrValue *result) {
    if (!ctx || !ctx->conn || !atomic_load(&ctx->cluster->running) ||
        cluster_now_ms() >= ctx->deadline_ms)
        return XR_CFUNC_DONE;

    for (int operations = 0; operations < 32; operations++) {
        if (ctx->phase == XR_INBOUND_TLS) {
            int tls_result = xr_tls_conn_handshake_server_try(ctx->conn->tls);
            if (tls_result == 1)
                return cluster_inbound_wait(X, ctx, XR_WAIT_READ, result);
            if (tls_result == 2)
                return cluster_inbound_wait(X, ctx, XR_WAIT_WRITE, result);
            if (tls_result != 0)
                return XR_CFUNC_DONE;
            ctx->conn->is_tls = true;
            cluster_inbound_reset_read(ctx, XR_INBOUND_READ_REQ_HEADER);
            continue;
        }

        if (ctx->phase == XR_INBOUND_WRITE_ACK) {
            int wait_events = XR_WAIT_WRITE;
            int n = cluster_conn_write_try(ctx->conn, ctx->write_buf + ctx->write_used,
                                           ctx->write_len - ctx->write_used, &wait_events);
            if (n == -1)
                return cluster_inbound_wait(X, ctx, wait_events, result);
            if (n <= 0)
                return XR_CFUNC_DONE;
            ctx->write_used += (size_t) n;
            if (ctx->write_used < ctx->write_len)
                continue;
            cluster_inbound_reset_read(ctx, XR_INBOUND_READ_DONE_HEADER);
            continue;
        }

        bool reading_header =
            ctx->phase == XR_INBOUND_READ_REQ_HEADER || ctx->phase == XR_INBOUND_READ_DONE_HEADER;
        uint8_t *target =
            reading_header ? ctx->header + ctx->header_used : ctx->payload + ctx->payload_used;
        size_t remaining = reading_header ? sizeof(ctx->header) - ctx->header_used
                                          : (size_t) ctx->payload_len - ctx->payload_used;
        int wait_events = XR_WAIT_READ;
        int n = cluster_conn_read_try(ctx->conn, target, remaining, &wait_events);
        if (n == -1)
            return cluster_inbound_wait(X, ctx, wait_events, result);
        if (n <= 0)
            return XR_CFUNC_DONE;

        if (reading_header) {
            ctx->header_used += (size_t) n;
            if (ctx->header_used < sizeof(ctx->header))
                continue;
            if (cluster_frame_read_header(ctx->header, sizeof(ctx->header), &ctx->frame_type,
                                          &ctx->payload_len) != 0 ||
                ctx->payload_len > sizeof(ctx->payload))
                return XR_CFUNC_DONE;
            if (ctx->phase == XR_INBOUND_READ_REQ_HEADER) {
                if (ctx->frame_type != XR_FRAME_HANDSHAKE_REQ)
                    return XR_CFUNC_DONE;
                ctx->phase = XR_INBOUND_READ_REQ_PAYLOAD;
            } else {
                if (ctx->frame_type != XR_FRAME_HANDSHAKE_DONE)
                    return XR_CFUNC_DONE;
                ctx->phase = XR_INBOUND_READ_DONE_PAYLOAD;
            }
            continue;
        }

        ctx->payload_used += (uint32_t) n;
        if (ctx->payload_used < ctx->payload_len)
            continue;
        if (ctx->phase == XR_INBOUND_READ_DONE_PAYLOAD)
            return cluster_inbound_complete(ctx);

        if (cluster_frame_decode_handshake_req(ctx->payload, ctx->payload_len, &ctx->request) !=
                0 ||
            ctx->request.version != XR_CLUSTER_HANDSHAKE_VERSION)
            return XR_CFUNC_DONE;

        memset(&ctx->ack, 0, sizeof(ctx->ack));
        ctx->ack.version = XR_CLUSTER_HANDSHAKE_VERSION;
        strncpy(ctx->ack.name, ctx->cluster->self_name, XR_NODE_NAME_MAX);
        xr_random_bytes(ctx->ack.nonce, XR_NONCE_SIZE);
        cluster_compute_proof(ctx->cluster->secret, ctx->request.nonce, ctx->ack.proof);
        ctx->ack.flags = 0x01;
        int frame_len =
            cluster_frame_encode_handshake_ack(ctx->write_buf, sizeof(ctx->write_buf), &ctx->ack);
        if (frame_len <= 0)
            return XR_CFUNC_DONE;
        ctx->write_len = (size_t) frame_len;
        ctx->write_used = 0;
        ctx->phase = XR_INBOUND_WRITE_ACK;
    }

    return xr_yield(X, cluster_inbound_continue, ctx);
}

static XrCFuncResult cluster_inbound_entry(XrVMRuntime *X, void *context, XrValue *result) {
    return cluster_inbound_drive(X, (XrInboundContext *) context, result);
}

static bool cluster_spawn_inbound(XrCluster *c, int fd) {
    XrInboundContext *ctx = (XrInboundContext *) xr_calloc(1, sizeof(XrInboundContext));
    if (!ctx) {
        xr_closesocket(fd);
        return false;
    }
    ctx->cluster = c;
    ctx->conn = xr_io_conn_from_fd(c->isolate, fd, XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
    ctx->deadline_ms = cluster_now_ms() + XR_CLUSTER_HANDSHAKE_TIMEOUT_MS;
    ctx->phase = XR_INBOUND_READ_REQ_HEADER;
    if (!ctx->conn) {
        xr_closesocket(fd);
        xr_free(ctx);
        return false;
    }
    if (c->tls_enabled) {
        ctx->conn->tls = xr_tls_conn_new(c->tls_server_ctx, fd);
        if (!ctx->conn->tls) {
            xr_io_close(ctx->conn);
            xr_free(ctx);
            return false;
        }
        ctx->phase = XR_INBOUND_TLS;
    }

    cluster_runtime_retain(c);
    XrCoroutine *coro =
        xr_coro_create_native_yieldable(c->isolate, cluster_inbound_entry, ctx,
                                        cluster_inbound_context_destroy, "cluster_handshake");
    if (!coro)
        return false;
    xr_coro_spawn(c->isolate, coro);
    return true;
}

static void cluster_accept_context_destroy(void *context) {
    XrCluster *c = (XrCluster *) context;
    if (!c)
        return;
    atomic_store(&c->accept_running, false);
    cluster_runtime_release(c);
}

static XrCFuncResult cluster_accept_drive(XrVMRuntime *X, XrCluster *c, XrValue *result);

static XrCFuncResult cluster_accept_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *context, XrValue *result) {
    (void) resume_value;
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return cluster_accept_drive(X, (XrCluster *) context, result);
}

static XrCFuncResult cluster_accept_drive(XrVMRuntime *X, XrCluster *c, XrValue *result) {
    if (!c || !atomic_load(&c->running) || (c->tls_enabled && !c->tls_server_ctx))
        return XR_CFUNC_DONE;

    for (int accepted = 0; accepted < 32; accepted++) {
        XrIOTryResult accept_result = xr_socket_accept_try(X, c->listen_fd);
        if (!accept_result.ready) {
            return xr_yield_for_io(X, c->listen_fd, XR_WAIT_READ, -1, cluster_accept_continue, c,
                                   result);
        }
        if (accept_result.error != 0 || accept_result.value < 0) {
            if (!atomic_load(&c->running))
                return XR_CFUNC_DONE;
            return xr_yield_for_timeout(X, 10, cluster_accept_continue, c, result);
        }
        (void) cluster_spawn_inbound(c, accept_result.value);
    }
    return xr_yield(X, cluster_accept_continue, c);
}

static XrCFuncResult cluster_accept_entry(XrVMRuntime *X, void *context, XrValue *result) {
    XrCluster *c = (XrCluster *) context;
    if (c)
        atomic_store(&c->accept_running, true);
    return cluster_accept_drive(X, c, result);
}

/* ========== Cluster Lifecycle ========== */

/*
 * Build the per-cluster TLS contexts from XrClusterTlsOptions.
 * Returns 0 on success. On failure any partially-allocated contexts are
 * freed and the corresponding XrCluster fields are left NULL.
 *
 * We keep the helper private to cluster.c because the resulting contexts
 * are owned by XrCluster; exposing creation would invite double-free
 * foot-guns from embedders. Callers get policy via XrClusterTlsOptions
 * instead of raw SSL_CTX * handles.
 */
static int build_cluster_tls(XrCluster *c, const XrClusterTlsOptions *opts) {
    // Client context covers outgoing cluster_runtime_join traffic.
    XrTlsContext *client_ctx = xr_tls_context_new_client();
    if (!client_ctx)
        return -1;

    if (opts->ca_file) {
        if (xr_tls_context_load_ca(client_ctx, opts->ca_file) != 0) {
            xr_tls_context_free(client_ctx);
            return -1;
        }
    }
    if (opts->insecure) {
        // Disable peer verification. Noisy on purpose — a failing mutual
        // auth rollout should be visible in the startup logs of every
        // affected node rather than silently downgrade.
        xr_tls_context_set_verify(client_ctx, false);
    }
    c->tls_client_ctx = client_ctx;

    // Server context is optional: only builds when cert+key supplied.
    // When absent, cluster_accept_loop refuses inbound TLS traffic outright
    // rather than silently downgrading to plaintext (see the
    // `tls_enabled && !tls_server_ctx` branch in cluster_accept_loop).
    if (opts->cert_file && opts->key_file) {
        XrTlsContext *server_ctx = xr_tls_context_new_server(opts->cert_file, opts->key_file);
        if (!server_ctx) {
            xr_tls_context_free(client_ctx);
            c->tls_client_ctx = NULL;
            return -1;
        }
        // A server context that also verifies the peer cert yields
        // mutual TLS. Cluster's threat model makes peer-as-attacker
        // plausible (one compromised node), so default to verify on.
        if (!opts->insecure && opts->ca_file) {
            xr_tls_context_load_ca(server_ctx, opts->ca_file);
            xr_tls_context_set_verify(server_ctx, true);
        }
        c->tls_server_ctx = server_ctx;
    }

    c->tls_enabled = true;
    return 0;
}

int cluster_runtime_start(XrVMRuntime *X, const char *name, uint16_t port, const char *secret,
                          const XrClusterTlsOptions *tls) {
    if (X->cluster)
        return -1;  // already running
    if (!name || name[0] == '\0')
        return -1;  // name required

    // Validate name: printable ASCII, max XR_NODE_NAME_MAX bytes
    size_t name_len = strlen(name);
    if (name_len > XR_NODE_NAME_MAX)
        return -1;
    for (size_t i = 0; i < name_len; i++) {
        if ((unsigned char) name[i] < 0x20 || (unsigned char) name[i] > 0x7E)
            return -1;
    }

    XrCluster *c = (XrCluster *) xr_calloc(1, sizeof(XrCluster));
    if (!c)
        return -1;

    atomic_store(&c->ref_count, 1);
    atomic_store(&c->stop_started, false);
    strncpy(c->self_name, name, XR_NODE_NAME_MAX);
    c->self_name[XR_NODE_NAME_MAX] = '\0';
    c->listen_port = port;
    if (secret) {
        strncpy(c->secret, secret, sizeof(c->secret) - 1);
    }
    c->isolate = X;

    // TLS bootstrap — must run before xr_io_listen so the accept loop (when
    // it is wired up) sees a ready server_ctx. Failures here are fatal to
    // startup: operators who asked for TLS explicitly would rather see a
    // loud error than silently fall back to plaintext.
    if (tls && tls->enabled) {
        if (build_cluster_tls(c, tls) != 0) {
            xr_secure_wipe(c->secret, sizeof(c->secret));
            xr_free(c);
            return -1;
        }
    }

    xr_amutex_init(&c->nodes_lock);
    xr_amutex_init(&c->dead_nodes_lock);
    xr_amutex_init(&c->topics_lock);

    /* Topic routing trie — allocated eagerly so subscribe never has to
     * worry about a NULL root under the lock. Failure here is fatal to
     * start: pub/sub is a first-class feature, not a best-effort add-on. */
    if (cluster_topics_init(c) != 0) {
        if (c->tls_client_ctx)
            xr_tls_context_free(c->tls_client_ctx);
        if (c->tls_server_ctx)
            xr_tls_context_free(c->tls_server_ctx);
        xr_secure_wipe(c->secret, sizeof(c->secret));
        xr_free(c);
        return -1;
    }

    c->heartbeat_interval_ms = 5000;
    c->heartbeat_timeout_ms = 15000;
    c->max_missed_heartbeats = 3;
    // Dynamic tombstone array
    c->tombstone_cap = 16;
    c->tombstones = xr_calloc((size_t) c->tombstone_cap, sizeof(c->tombstones[0]));
    c->tombstone_count = 0;
    c->monitors = NULL;
    c->monitor_count = 0;
    xr_amutex_init(&c->monitors_lock);

    // Start listening
    c->listen_fd = xr_io_listen(NULL, port, 128);
    if (c->listen_fd < 0) {
        // Release TLS contexts that build_cluster_tls may have created
        // before the listen failure so we do not leak OpenSSL handles.
        if (c->tls_client_ctx)
            xr_tls_context_free(c->tls_client_ctx);
        if (c->tls_server_ctx)
            xr_tls_context_free(c->tls_server_ctx);
        xr_secure_wipe(c->secret, sizeof(c->secret));
        xr_free(c);
        return -1;
    }

    atomic_store(&c->running, true);

    X->cluster = c;

    XrCoroutine *background[2];
    int background_count = 0;

    /* Heartbeat and accept are published together below. Batch placement is
     * part of the lifecycle contract: two independent local-runnext spawns can
     * leave a long-lived blocking accept callback in front of its peer. */
    cluster_runtime_retain(c);
    XrCoroutine *hb_coro = xr_coro_create_native_yieldable(
        X, cluster_heartbeat_entry, c, cluster_heartbeat_context_destroy, "cluster_heartbeat");
    if (hb_coro) {
        background[background_count++] = hb_coro;
        c->heartbeat_coro_spawned = true;
    }

    /*
     * Spawn the inbound-accept coroutine. Failure here is non-fatal
     * for outbound-only deployments (think: edge nodes that only
     * initiate to a central core), but we still surface it via the
     * accept_coro_spawned flag so cluster_runtime_stop does not wait for
     * something that never ran.
     */
    cluster_runtime_retain(c);
    XrCoroutine *accept_coro = xr_coro_create_native_yieldable(
        X, cluster_accept_entry, c, cluster_accept_context_destroy, "cluster_accept");
    if (accept_coro) {
        background[background_count++] = accept_coro;
        c->accept_coro_spawned = true;
    }

    XrRuntime *runtime = (XrRuntime *) X->vm.scheduler;
    if (background_count != 2 || !runtime) {
        for (int i = 0; i < background_count; i++) {
            xr_coro_destroy(background[i]);
        }
        c->heartbeat_coro_spawned = false;
        c->accept_coro_spawned = false;
        cluster_runtime_stop(c);
        return -1;
    }
    xr_runtime_spawn_batch(runtime, background, background_count);

    return 0;
}

void cluster_runtime_retain(XrCluster *c) {
    XR_DCHECK(c != NULL, "cluster retain requires a runtime");
    if (c)
        atomic_fetch_add(&c->ref_count, 1);
}

static void cluster_runtime_destroy(XrCluster *c) {
    XR_DCHECK(c != NULL, "cluster destroy requires a runtime");
    XR_DCHECK(c->nodes == NULL, "cluster destroy requires a detached node list");

    cluster_discovery_stop(c);
    cluster_topics_destroy(c);

    xr_amutex_lock(&c->monitors_lock);
    XrNodeMonitor *mon = c->monitors;
    while (mon) {
        XrNodeMonitor *next = mon->next;
        xr_free(mon);
        mon = next;
    }
    c->monitors = NULL;
    c->monitor_count = 0;
    xr_amutex_unlock(&c->monitors_lock);

    XrRemoteCoroMonitor *rm = c->remote_coro_monitors;
    while (rm) {
        XrRemoteCoroMonitor *next = rm->next;
        xr_free(rm);
        rm = next;
    }
    c->remote_coro_monitors = NULL;

    xr_free(c->tombstones);
    c->tombstones = NULL;

    if (c->tls_client_ctx)
        xr_tls_context_free(c->tls_client_ctx);
    if (c->tls_server_ctx)
        xr_tls_context_free(c->tls_server_ctx);
    c->tls_client_ctx = NULL;
    c->tls_server_ctx = NULL;
    c->tls_enabled = false;

    xr_secure_wipe(c->secret, sizeof(c->secret));
    if (c->listen_fd >= 0)
        close(c->listen_fd);
    c->listen_fd = -1;
    xr_free(c);
}

void cluster_runtime_release(XrCluster *c) {
    if (!c)
        return;
    uint32_t previous = atomic_fetch_sub(&c->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster reference underflow");
    if (previous == 1)
        cluster_runtime_destroy(c);
}

void cluster_runtime_stop(XrCluster *c) {
    if (!c)
        return;
    if (atomic_exchange(&c->stop_started, true))
        return;

    atomic_store(&c->running, false);
    if (c->isolate && c->isolate->cluster == c)
        c->isolate->cluster = NULL;

    /*
     * Close the listen socket early so the accept coroutine wakes up
     * with EBADF on its next accept() and observes running=false at
     * the top of its loop. Doing this before node teardown below
     * prevents a race where a freshly accepted peer would race against
     * the cleanup sweep.
     */
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        c->listen_fd = -1;
    }

    /* Detach the list atomically, then close nodes without holding
     * nodes_lock. Reader and writer coroutines own independent references;
     * their final release performs destruction after they return. */
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    c->nodes = NULL;
    c->node_count = 0;
    xr_amutex_unlock(&c->nodes_lock);
    while (node) {
        XrClusterNode *next = node->next;
        node->next = NULL;
        cluster_node_shutdown(node);
        cluster_node_release(node);
        node = next;
    }

    /* Release the isolate-owned reference. Background coroutines keep the
     * runtime alive and the last one performs final destruction. */
    cluster_runtime_release(c);
}

bool cluster_runtime_is_running(XrCluster *c) {
    return c && atomic_load(&c->running);
}

const char *cluster_runtime_self_name(XrCluster *c) {
    return c ? c->self_name : "";
}

/* ========== Node Management ========== */

XrClusterNode *cluster_node_find(XrCluster *c, const char *name) {
    if (!c)
        return NULL;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            cluster_node_retain(node);
            xr_amutex_unlock(&c->nodes_lock);
            return node;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    return NULL;
}

bool cluster_node_add(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return false;

    xr_amutex_lock(&c->nodes_lock);
    if (!atomic_load(&c->running)) {
        xr_amutex_unlock(&c->nodes_lock);
        return false;
    }
    node->next = c->nodes;
    c->nodes = node;
    c->node_count++;
    xr_amutex_unlock(&c->nodes_lock);
    return true;
}

bool cluster_node_remove(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return false;

    bool removed = false;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode **pp = &c->nodes;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next;
            node->next = NULL;
            c->node_count--;
            removed = true;
            break;
        }
        pp = &(*pp)->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    return removed;
}

typedef enum XrJoinPhase {
    XR_JOIN_CONNECT_START,
    XR_JOIN_CONNECT_WAIT,
    XR_JOIN_TLS,
    XR_JOIN_WRITE_REQUEST,
    XR_JOIN_READ_ACK_HEADER,
    XR_JOIN_READ_ACK_PAYLOAD,
    XR_JOIN_WRITE_DONE,
} XrJoinPhase;

typedef struct XrJoinContext {
    XrCluster *cluster;
    XrClusterNode *node;
    bool coroutine_owned;
    XrJoinPhase phase;
    int64_t deadline_ms;
    int fd;
    XrSockAddr addresses[XR_DNS_MAX_ADDRS];
    int address_count;
    int address_index;
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    size_t header_used;
    uint8_t payload[512];
    uint32_t payload_len;
    uint32_t payload_used;
    uint8_t frame_type;
    uint8_t write_buf[512];
    size_t write_len;
    size_t write_used;
    XrFrameHandshakeReq request;
    XrFrameHandshakeAck ack;
    XrFrameHandshakeDone done;
} XrJoinContext;

static void cluster_join_context_destroy(void *context) {
    XrJoinContext *ctx = (XrJoinContext *) context;
    if (!ctx)
        return;
    if (ctx->fd >= 0) {
        xr_closesocket((xr_socket_t) ctx->fd);
        ctx->fd = -1;
    }
    if (ctx->node) {
        cluster_node_shutdown(ctx->node);
        cluster_node_release(ctx->node);
    }
    xr_secure_wipe(&ctx->request, sizeof(ctx->request));
    xr_secure_wipe(&ctx->ack, sizeof(ctx->ack));
    xr_secure_wipe(&ctx->done, sizeof(ctx->done));
    xr_secure_wipe(ctx->payload, sizeof(ctx->payload));
    xr_secure_wipe(ctx->write_buf, sizeof(ctx->write_buf));
    cluster_runtime_release(ctx->cluster);
    xr_free(ctx);
}

static XrCFuncResult cluster_join_finish(XrJoinContext *ctx, bool success, XrValue *result) {
    *result = xr_bool(success);
    if (!ctx->coroutine_owned)
        cluster_join_context_destroy(ctx);
    return XR_CFUNC_DONE;
}

static bool cluster_join_adopt_fd(XrJoinContext *ctx) {
    XrIOConn *conn = (XrIOConn *) xr_calloc(1, sizeof(*conn));
    if (!conn)
        return false;
    conn->fd = ctx->fd;
    conn->timeout_ms = XR_CLUSTER_HANDSHAKE_TIMEOUT_MS;
    conn->last_error = XR_NERR_OK;
    conn->X = ctx->cluster->isolate;
    ctx->fd = -1;
    ctx->node->conn = conn;
    ctx->node->state = XR_NODE_HANDSHAKING;

    if (ctx->cluster->tls_enabled) {
        if (!ctx->cluster->tls_client_ctx)
            return false;
        conn->tls = xr_tls_conn_new(ctx->cluster->tls_client_ctx, conn->fd);
        if (!conn->tls || xr_tls_conn_set_hostname(conn->tls, ctx->node->host) != 0)
            return false;
        ctx->phase = XR_JOIN_TLS;
    } else {
        ctx->phase = XR_JOIN_WRITE_REQUEST;
    }
    return true;
}

static int cluster_join_open_next(XrJoinContext *ctx) {
    ctx->node->state = XR_NODE_CONNECTING;
    while (ctx->address_index < ctx->address_count) {
        XrSockAddr address = ctx->addresses[ctx->address_index++];
        int fd = (int) socket(address.family, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        xr_socket_set_nonblocking((xr_socket_t) fd);
        xr_socket_set_nodelay((xr_socket_t) fd, true);

        struct sockaddr *socket_address = NULL;
        socklen_t socket_address_len = 0;
        if (address.family == AF_INET) {
            address.addr.v4.sin_port = htons(ctx->node->port);
            socket_address = (struct sockaddr *) &address.addr.v4;
            socket_address_len = sizeof(address.addr.v4);
        } else if (address.family == AF_INET6) {
            address.addr.v6.sin6_port = htons(ctx->node->port);
            socket_address = (struct sockaddr *) &address.addr.v6;
            socket_address_len = sizeof(address.addr.v6);
        } else {
            xr_closesocket((xr_socket_t) fd);
            continue;
        }

        int connect_result = connect(fd, socket_address, socket_address_len);
        if (connect_result == 0) {
            ctx->fd = fd;
            return cluster_join_adopt_fd(ctx) ? 0 : -1;
        }
        int error = xr_get_socket_error();
        if (xr_socket_err_is_inprogress(error)) {
            ctx->fd = fd;
            ctx->phase = XR_JOIN_CONNECT_WAIT;
            return 1;
        }
        xr_closesocket((xr_socket_t) fd);
    }
    return -1;
}

static bool cluster_join_prepare_request(XrJoinContext *ctx) {
    if (ctx->write_len != 0)
        return true;
    memset(&ctx->request, 0, sizeof(ctx->request));
    ctx->request.version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(ctx->request.name, ctx->cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(ctx->request.nonce, XR_NONCE_SIZE);
    ctx->request.flags = 0x01;
    int frame_len =
        cluster_frame_encode_handshake_req(ctx->write_buf, sizeof(ctx->write_buf), &ctx->request);
    if (frame_len < 0)
        return false;
    ctx->write_len = (size_t) frame_len;
    return true;
}

static void cluster_join_reset_read(XrJoinContext *ctx, XrJoinPhase phase) {
    ctx->phase = phase;
    ctx->header_used = 0;
    ctx->payload_len = 0;
    ctx->payload_used = 0;
    ctx->frame_type = 0;
}

static XrCFuncResult cluster_join_drive(XrVMRuntime *X, XrJoinContext *ctx, XrValue *result);

static XrCFuncResult cluster_join_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                           void *context, XrValue *result) {
    (void) resume_value;
    XrJoinContext *ctx = (XrJoinContext *) context;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return cluster_join_finish(ctx, false, result);
    return cluster_join_drive(X, ctx, result);
}

static XrCFuncResult cluster_join_wait(XrVMRuntime *X, XrJoinContext *ctx, int events,
                                       XrValue *result) {
    int64_t remaining = ctx->deadline_ms - cluster_now_ms();
    if (remaining <= 0)
        return cluster_join_finish(ctx, false, result);
    int fd = ctx->node && ctx->node->conn ? ctx->node->conn->fd : ctx->fd;
    return xr_yield_for_io(X, fd, events, remaining, cluster_join_continue, ctx, result);
}

static bool cluster_join_verify_ack(XrJoinContext *ctx) {
    if (cluster_frame_decode_handshake_ack(ctx->payload, ctx->payload_len, &ctx->ack) != 0 ||
        ctx->ack.version != XR_CLUSTER_HANDSHAKE_VERSION)
        return false;
    uint8_t expected_proof[XR_PROOF_SIZE];
    cluster_compute_proof(ctx->cluster->secret, ctx->request.nonce, expected_proof);
    bool valid = cluster_proof_equal(ctx->ack.proof, expected_proof);
    xr_secure_wipe(expected_proof, sizeof(expected_proof));
    if (!valid)
        return false;

    strncpy(ctx->node->name, ctx->ack.name, XR_NODE_NAME_MAX);
    ctx->node->name[XR_NODE_NAME_MAX] = '\0';
    ctx->node->flags = ctx->ack.flags;
    cluster_compute_proof(ctx->cluster->secret, ctx->ack.nonce, ctx->done.proof);
    int frame_len =
        cluster_frame_encode_handshake_done(ctx->write_buf, sizeof(ctx->write_buf), &ctx->done);
    if (frame_len < 0)
        return false;
    ctx->write_len = (size_t) frame_len;
    ctx->write_used = 0;
    ctx->phase = XR_JOIN_WRITE_DONE;
    return true;
}

static XrCFuncResult cluster_join_publish(XrJoinContext *ctx, XrValue *result) {
    XrClusterNode *node = ctx->node;
    node->state = XR_NODE_CONNECTED;
    node->last_heartbeat_recv = cluster_now_ms();
    if (cluster_health_is_dead(ctx->cluster, node->name) || !cluster_node_add(ctx->cluster, node))
        return cluster_join_finish(ctx, false, result);
    if (!cluster_node_start_io(ctx->cluster, node)) {
        (void) cluster_node_remove(ctx->cluster, node);
        cluster_node_shutdown(node);
        cluster_node_release(node);
        ctx->node = NULL;
        return cluster_join_finish(ctx, false, result);
    }
    ctx->node = NULL;
    return cluster_join_finish(ctx, true, result);
}

static XrCFuncResult cluster_join_drive(XrVMRuntime *X, XrJoinContext *ctx, XrValue *result) {
    if (!ctx || !atomic_load(&ctx->cluster->running) || cluster_now_ms() >= ctx->deadline_ms)
        return cluster_join_finish(ctx, false, result);

    for (int operations = 0; operations < 32; operations++) {
        if (ctx->phase == XR_JOIN_CONNECT_START) {
            int open_result = cluster_join_open_next(ctx);
            if (open_result < 0)
                return cluster_join_finish(ctx, false, result);
            if (open_result > 0)
                return cluster_join_wait(X, ctx, XR_WAIT_WRITE, result);
            continue;
        }
        if (ctx->phase == XR_JOIN_CONNECT_WAIT) {
            int error = xr_socket_get_error((xr_socket_t) ctx->fd);
            if (error != 0) {
                xr_closesocket((xr_socket_t) ctx->fd);
                ctx->fd = -1;
                ctx->phase = XR_JOIN_CONNECT_START;
                continue;
            }
            if (!cluster_join_adopt_fd(ctx))
                return cluster_join_finish(ctx, false, result);
            continue;
        }
        if (ctx->phase == XR_JOIN_TLS) {
            int tls_result = xr_tls_conn_handshake_try(ctx->node->conn->tls);
            if (tls_result == 1)
                return cluster_join_wait(X, ctx, XR_WAIT_READ, result);
            if (tls_result == 2)
                return cluster_join_wait(X, ctx, XR_WAIT_WRITE, result);
            if (tls_result != 0)
                return cluster_join_finish(ctx, false, result);
            ctx->node->conn->is_tls = true;
            ctx->phase = XR_JOIN_WRITE_REQUEST;
            continue;
        }
        if (ctx->phase == XR_JOIN_WRITE_REQUEST || ctx->phase == XR_JOIN_WRITE_DONE) {
            if (ctx->phase == XR_JOIN_WRITE_REQUEST && !cluster_join_prepare_request(ctx))
                return cluster_join_finish(ctx, false, result);
            int wait_events = XR_WAIT_WRITE;
            int written = cluster_conn_write_try(ctx->node->conn, ctx->write_buf + ctx->write_used,
                                                 ctx->write_len - ctx->write_used, &wait_events);
            if (written == -1)
                return cluster_join_wait(X, ctx, wait_events, result);
            if (written <= 0)
                return cluster_join_finish(ctx, false, result);
            ctx->write_used += (size_t) written;
            if (ctx->write_used < ctx->write_len)
                continue;
            if (ctx->phase == XR_JOIN_WRITE_DONE)
                return cluster_join_publish(ctx, result);
            ctx->write_len = 0;
            ctx->write_used = 0;
            cluster_join_reset_read(ctx, XR_JOIN_READ_ACK_HEADER);
            continue;
        }

        bool reading_header = ctx->phase == XR_JOIN_READ_ACK_HEADER;
        uint8_t *target =
            reading_header ? ctx->header + ctx->header_used : ctx->payload + ctx->payload_used;
        size_t remaining = reading_header ? sizeof(ctx->header) - ctx->header_used
                                          : (size_t) ctx->payload_len - ctx->payload_used;
        int wait_events = XR_WAIT_READ;
        int read_count = cluster_conn_read_try(ctx->node->conn, target, remaining, &wait_events);
        if (read_count == -1)
            return cluster_join_wait(X, ctx, wait_events, result);
        if (read_count <= 0)
            return cluster_join_finish(ctx, false, result);
        if (reading_header) {
            ctx->header_used += (size_t) read_count;
            if (ctx->header_used < sizeof(ctx->header))
                continue;
            if (cluster_frame_read_header(ctx->header, sizeof(ctx->header), &ctx->frame_type,
                                          &ctx->payload_len) != 0 ||
                ctx->frame_type != XR_FRAME_HANDSHAKE_ACK ||
                ctx->payload_len > sizeof(ctx->payload))
                return cluster_join_finish(ctx, false, result);
            ctx->phase = XR_JOIN_READ_ACK_PAYLOAD;
            continue;
        }
        ctx->payload_used += (uint32_t) read_count;
        if (ctx->payload_used < ctx->payload_len)
            continue;
        if (!cluster_join_verify_ack(ctx))
            return cluster_join_finish(ctx, false, result);
    }
    return xr_yield(X, cluster_join_continue, ctx);
}

static XrJoinContext *cluster_join_context_new(XrCluster *cluster, const char *host, uint16_t port,
                                               bool coroutine_owned) {
    if (!cluster || !host || !host[0] || !port || !atomic_load(&cluster->running))
        return NULL;
    XrJoinContext *ctx = (XrJoinContext *) xr_calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->cluster = cluster;
    ctx->coroutine_owned = coroutine_owned;
    ctx->phase = XR_JOIN_CONNECT_START;
    ctx->deadline_ms = cluster_now_ms() + XR_CLUSTER_HANDSHAKE_TIMEOUT_MS;
    ctx->fd = -1;
    ctx->node = cluster_node_new(NULL, host, port);
    if (!ctx->node) {
        xr_free(ctx);
        return NULL;
    }
    ctx->address_count =
        xr_dns_resolve_all(cluster->isolate, host, ctx->addresses, XR_DNS_MAX_ADDRS, XR_AF_UNSPEC);
    if (ctx->address_count <= 0) {
        cluster_node_release(ctx->node);
        xr_free(ctx);
        return NULL;
    }
    cluster_runtime_retain(cluster);
    return ctx;
}

static XrCFuncResult cluster_join_entry(XrVMRuntime *X, void *context, XrValue *result) {
    return cluster_join_drive(X, (XrJoinContext *) context, result);
}

bool cluster_runtime_join_spawn(XrCluster *cluster, const char *host, uint16_t port) {
    XrJoinContext *ctx = cluster_join_context_new(cluster, host, port, true);
    if (!ctx)
        return false;
    XrCoroutine *coro = xr_coro_create_native_yieldable(
        cluster->isolate, cluster_join_entry, ctx, cluster_join_context_destroy, "cluster_join");
    if (!coro) {
        cluster_join_context_destroy(ctx);
        return false;
    }
    xr_coro_spawn(cluster->isolate, coro);
    return true;
}

/* ========== xray Function Bindings ========== */

// The pure-Xray public wrapper normalizes ClusterConfig into scalar values so
// both backends consume one representation-independent runtime boundary.
static XrValue cluster_start_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 8 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_STRING(args[2]) ||
        !XR_IS_BOOL(args[3]) || !XR_IS_STRING(args[4]) || !XR_IS_STRING(args[5]) ||
        !XR_IS_STRING(args[6]) || !XR_IS_BOOL(args[7]))
        return xr_bool(false);

    XrString *name = XR_TO_STRING(args[0]);
    int64_t port_value = XR_TO_INT(args[1]);
    if (port_value < 0 || port_value > UINT16_MAX)
        return xr_bool(false);
    uint16_t port = (uint16_t) port_value;
    const char *secret = XR_TO_STRING(args[2])->data;

    XrClusterTlsOptions tls_opts;
    memset(&tls_opts, 0, sizeof(tls_opts));
    const XrClusterTlsOptions *tls_ptr = NULL;
    if (XR_TO_BOOL(args[3])) {
        const char *ca_file = XR_TO_STRING(args[4])->data;
        const char *cert_file = XR_TO_STRING(args[5])->data;
        const char *key_file = XR_TO_STRING(args[6])->data;
        tls_opts.enabled = true;
        tls_opts.ca_file = ca_file[0] ? ca_file : NULL;
        tls_opts.cert_file = cert_file[0] ? cert_file : NULL;
        tls_opts.key_file = key_file[0] ? key_file : NULL;
        tls_opts.insecure = XR_TO_BOOL(args[7]);
        tls_ptr = &tls_opts;
    }

    int rc = cluster_runtime_start(X, name->data, port, secret, tls_ptr);
    return xr_bool(rc == 0);
}

// cluster.join(addr) - netpoll-driven connect and mutual handshake.
static XrCFuncResult cluster_join(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || argc < 1 || !XR_IS_STRING(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrString *addr = XR_TO_STRING(args[0]);
    char host[256] = {0};

    // Parse one canonical host:port form. Empty hosts, partial numeric ports,
    // and port zero fail before any socket or continuation is allocated.
    const char *colon = strrchr(addr->data, ':');
    if (!colon || colon == addr->data || colon[1] == '\0') {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    size_t host_len = (size_t) (colon - addr->data);
    if (host_len >= sizeof(host)) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    memcpy(host, addr->data, host_len);
    host[host_len] = '\0';
    char *end = NULL;
    errno = 0;
    long port_value = strtol(colon + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || port_value <= 0 || port_value > UINT16_MAX) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrJoinContext *ctx = cluster_join_context_new(c, host, (uint16_t) port_value, false);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    return cluster_join_drive(X, ctx, result);
}

// cluster.self() - returns node name
static XrValue cluster_self(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    const char *name = cluster_runtime_self_name(c);
    XrString *str = xr_string_intern(X, name, (uint32_t) strlen(name), 0);
    return xr_string_value(str);
}

// cluster.nodes() - returns array of connected node names
static XrValue cluster_nodes(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrArray *arr = xr_array_new(NULL);
    if (!arr)
        return xr_null();

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        XrString *name = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
        xr_array_push(arr, xr_string_value(name));
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    return xr_value_from_array(arr);
}

// cluster.discover() - start LAN auto-discovery via UDP multicast
static XrValue cluster_discover_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_bool(0);

    int rc = cluster_discovery_start(c);
    return xr_bool(rc == 0);
}

// cluster.stop()
static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    cluster_runtime_stop((XrCluster *) X->cluster);
    return xr_null();
}

/* ========== Frame Processing ========== */

void cluster_process_frame(XrCluster *c, XrClusterNode *node, uint8_t frame_type,
                           const uint8_t *payload, uint32_t payload_len) {
    XR_DCHECK(c != NULL, "cluster must be initialized");
    XR_DCHECK(node != NULL, "node must not be NULL");
    if (!c || !node)
        return;

    switch (frame_type) {
        case XR_FRAME_HEARTBEAT_PING: {
            // Reply with PONG via output queue
            int64_t ts;
            if (cluster_frame_decode_heartbeat(payload, payload_len, &ts) == 0) {
                uint8_t pong[32];
                int plen =
                    cluster_frame_encode_heartbeat(pong, sizeof(pong), XR_FRAME_HEARTBEAT_PONG, ts);
                if (plen > 0) {
                    cluster_node_enqueue(node, pong, (uint32_t) plen);
                }
            }
            int64_t now_hb = cluster_now_ms();
            node->last_heartbeat_recv = now_hb;
            node->missed_heartbeats = 0;
            cluster_phi_record_heartbeat(&node->phi, now_hb);
            break;
        }

        case XR_FRAME_HEARTBEAT_PONG: {
            int64_t now_pong = cluster_now_ms();
            // Compute RTT from ping timestamp
            int64_t ping_ts;
            if (cluster_frame_decode_heartbeat(payload, payload_len, &ping_ts) == 0) {
                node->metrics.last_rtt_ms = now_pong - ping_ts;
            }
            node->last_heartbeat_recv = now_pong;
            node->missed_heartbeats = 0;
            cluster_phi_record_heartbeat(&node->phi, now_pong);
            break;
        }

        case XR_FRAME_TRANSPORT_ENVELOPE: {
            /*
             * Wire format:
             *   [hop 1B] [topic_len 1B] [topic ...] [value_data ...]
             */
            if (payload_len >= 2) {
                uint8_t hop_limit = payload[0];
                uint8_t topic_len = payload[1];
                if (topic_len > 0 && 2 + topic_len < payload_len) {
                    char topic[XR_TOPIC_PATTERN_MAX + 1];
                    if (topic_len <= XR_TOPIC_PATTERN_MAX) {
                        memcpy(topic, payload + 2, topic_len);
                        topic[topic_len] = '\0';
                        uint32_t val_offset = 2 + topic_len;
                        uint32_t val_len = payload_len - val_offset;
                        cluster_transport_handle_frame(c, node, topic, payload + val_offset,
                                                       val_len, hop_limit);
                    }
                }
            }
            break;
        }

        case XR_FRAME_CORO_MONITOR: {
            char coro_name[XR_CORO_NAME_MAX + 1];
            if (cluster_frame_decode_coro_monitor(payload, payload_len, coro_name,
                                                  sizeof(coro_name)) == 0) {
                cluster_monitor_handle_coro_request(c, node, coro_name);
            }
            break;
        }

        case XR_FRAME_CORO_EXIT: {
            char coro_name[XR_CORO_NAME_MAX + 1];
            char reason[128];
            if (cluster_frame_decode_coro_exit(payload, payload_len, coro_name, sizeof(coro_name),
                                               reason, sizeof(reason)) == 0) {
                cluster_monitor_handle_coro_exit(c, coro_name, reason);
            }
            break;
        }

        case XR_FRAME_CORO_DEMONITOR:
            // Future: remove remote monitor
            break;

        default:
            break;
    }
}

static XrValue cluster_delivery_value(XrVMRuntime *X, XrClusterDelivery delivery) {
    XrEnumType *type = xr_stdlib_enum_type_get(X, "cluster", "ClusterDelivery");
    if (!type || delivery < XR_CLUSTER_DELIVERY_ACCEPTED ||
        delivery > XR_CLUSTER_DELIVERY_DISCONNECTED)
        return XR_NULL_VAL;
    XrEnumAggregateValue *value = xr_enum_zero_payload_value(
        xr_isolate_get_runtime_core(X), type, (uint32_t) delivery);
    return value ? XR_FROM_PTR(value) : XR_NULL_VAL;
}

// xray binding: cluster.send(topic, move envelope)
static XrValue cluster_send_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return cluster_delivery_value(X, XR_CLUSTER_DELIVERY_INVALID_TOPIC);

    const uint8_t *envelope = NULL;
    size_t envelope_len = 0;
    if (!xr_mem_buffer_bytes(args[1], &envelope, &envelope_len) || envelope_len > UINT32_MAX)
        return cluster_delivery_value(X, XR_CLUSTER_DELIVERY_INVALID_ENVELOPE);

    XrString *topic = XR_TO_STRING(args[0]);
    XrClusterDelivery delivery =
        cluster_transport_send(X, topic->data, envelope, (uint32_t) envelope_len);
    return cluster_delivery_value(X, delivery);
}

// xray binding: cluster.listen(pattern)
static XrValue cluster_listen_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();

    XrString *pattern_str = XR_TO_STRING(args[0]);
    int64_t capacity = XR_TO_INT(args[1]);
    if (capacity <= 0 || capacity > XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX)
        return xr_null();
    XrChannel *ch = cluster_transport_listen(X, pattern_str->data, (uint32_t) capacity);
    if (!ch)
        return xr_null();
    return xr_value_from_channel(ch);
}

/* ========== Cluster Info API ========== */

static XrObjectInstance *cluster_object_new(XrVMRuntime *X, const char *name) {
    XrClass *cls = xr_stdlib_record_class_get(X, "cluster", name);
    return cls ? xr_object_instance_new_with_class(NULL, cls) : NULL;
}

static XrValue cluster_node_state_value(XrVMRuntime *X, int state) {
    XrEnumType *type = xr_stdlib_enum_type_get(X, "cluster", "ClusterNodeState");
    if (!type || state < XR_NODE_IDLE || state > XR_NODE_CLOSING)
        return XR_NULL_VAL;
    XrEnumAggregateValue *value = xr_enum_zero_payload_value(
        xr_isolate_get_runtime_core(X), type, (uint32_t) state);
    return value ? XR_FROM_PTR(value) : XR_NULL_VAL;
}

static XrValue cluster_info_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrObjectInstance *info = cluster_object_new(X, "ClusterInfo");
    if (!info)
        return xr_null();

    // Self name
    XrString *self = xr_string_intern(X, c->self_name, (uint32_t) strlen(c->self_name), 0);
    xr_object_instance_set_by_key(X, info, "self", xr_string_value(self));
    xr_object_instance_set_by_key(X, info, "port", xr_int(c->listen_port));
    xr_object_instance_set_by_key(X, info, "running", xr_bool(atomic_load(&c->running)));

    // Node list with metrics
    XrArray *node_arr = xr_array_new(NULL);
    if (node_arr) {
        xr_amutex_lock(&c->nodes_lock);
        XrClusterNode *node = c->nodes;
        while (node) {
            XrObjectInstance *nj = cluster_object_new(X, "ClusterNodeInfo");
            if (nj) {
                XrString *nname = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
                xr_object_instance_set_by_key(X, nj, "name", xr_string_value(nname));

                XrString *nhost = xr_string_intern(X, node->host, (uint32_t) strlen(node->host), 0);
                xr_object_instance_set_by_key(X, nj, "host", xr_string_value(nhost));
                xr_object_instance_set_by_key(X, nj, "port", xr_int(node->port));
                XrValue state = cluster_node_state_value(X, node->state);
                if (XR_IS_NULL(state)) {
                    xr_amutex_unlock(&c->nodes_lock);
                    return xr_null();
                }
                xr_object_instance_set_by_key(X, nj, "state", state);

                /*
                 * Per-node metrics snapshot. All counters are
                 * atomic _Atomic(uint64_t) so the load is wait-free
                 * and consistent per-field (no struct-level tearing
                 * because each load is independent). A
                 * whole-metrics-block observation is NOT atomic —
                 * bytes_sent may advance after frames_sent is read,
                 * producing a momentarily-impossible ratio; the
                 * tradeoff is acceptable for a diagnostic JSON.
                 */
                xr_object_instance_set_by_key(
                    X, nj, "framesSent", xr_int((int64_t) atomic_load(&node->metrics.frames_sent)));
                xr_object_instance_set_by_key(
                    X, nj, "framesReceived",
                    xr_int((int64_t) atomic_load(&node->metrics.frames_recv)));
                xr_object_instance_set_by_key(
                    X, nj, "bytesSent", xr_int((int64_t) atomic_load(&node->metrics.bytes_sent)));
                xr_object_instance_set_by_key(
                    X, nj, "bytesReceived",
                    xr_int((int64_t) atomic_load(&node->metrics.bytes_recv)));
                // send_errors: writev short/fail counter — high values
                // flag a slow or lossy link; correlate with the slow
                // flag below.
                xr_object_instance_set_by_key(
                    X, nj, "sendErrors", xr_int((int64_t) atomic_load(&node->metrics.send_errors)));
                // slow_consumer_events: total times this peer hit the
                // high watermark (4 MiB by default) since start. Each
                // event corresponds to one outq_bytes >= high_watermark
                // transition in cluster_node.
                xr_object_instance_set_by_key(
                    X, nj, "slowConsumerEvents",
                    xr_int((int64_t) atomic_load(&node->metrics.slow_consumer_events)));
                xr_object_instance_set_by_key(X, nj, "rttMs", xr_int(node->metrics.last_rtt_ms));
                xr_object_instance_set_by_key(X, nj, "outQueueBytes",
                                              xr_int(node->outq.total_bytes));
                xr_object_instance_set_by_key(X, nj, "outQueueFrames",
                                              xr_int(node->outq.frame_count));
                xr_object_instance_set_by_key(X, nj, "slow", xr_bool(cluster_node_is_slow(node)));

                // Phi accrual failure-detector score. Higher = more
                // likely dead. Threshold for "kill" is set by
                // cluster policy in cluster_health.c.
                int64_t now = cluster_now_ms();
                double phi = cluster_phi_value(&node->phi, now);
                xr_object_instance_set_by_key(X, nj, "phi", xr_float(phi));
                xr_object_instance_set_by_key(X, nj, "missedHeartbeats",
                                              xr_int((int64_t) node->missed_heartbeats));

                xr_array_push(node_arr, xr_object_instance_value(nj));
            } else {
                xr_amutex_unlock(&c->nodes_lock);
                return xr_null();
            }
            node = node->next;
        }
        xr_amutex_unlock(&c->nodes_lock);
        xr_object_instance_set_by_key(X, info, "nodes", xr_value_from_array(node_arr));
    } else {
        return xr_null();
    }

    // Listener count is diagnostic and may be momentarily stale.
    xr_object_instance_set_by_key(X, info, "listeners", xr_int(c->topic_sub_count));

    /*
     * Tombstone snapshot — number of nodes in the recently-dead
     * table. A non-zero value across successive calls means we have
     * peers that left the cluster within the past
     * XR_TOMBSTONE_WINDOW_MS (see cluster_health.c) and will be
     * refused if they try to rejoin. Useful for correlating "split
     * brain" scenarios.
     */
    xr_amutex_lock(&c->dead_nodes_lock);
    xr_object_instance_set_by_key(X, info, "deadNodes", xr_int(c->tombstone_count));
    xr_amutex_unlock(&c->dead_nodes_lock);

    /*
     * Expose the operator-configurable heartbeat knobs so ops can
     * sanity-check the live cluster against their YAML without
     * shelling into the node. These fields are rarely changed at
     * runtime but live at the XrCluster level so a snapshot is
     * trivially consistent.
     */
    xr_object_instance_set_by_key(X, info, "heartbeatIntervalMs", xr_int(c->heartbeat_interval_ms));
    xr_object_instance_set_by_key(X, info, "heartbeatTimeoutMs", xr_int(c->heartbeat_timeout_ms));
    xr_object_instance_set_by_key(X, info, "maxMissedHeartbeats", xr_int(c->max_missed_heartbeats));

    /*
     * TLS posture is a typed nested object. A mis-configured cluster
     * (enabled with neither context ready) remains directly visible to
     * operators without exposing a bitmap convention in the public API.
     */
    XrObjectInstance *tls = cluster_object_new(X, "ClusterTlsStatus");
    if (!tls)
        return xr_null();
    xr_object_instance_set_by_key(X, tls, "enabled", xr_bool(c->tls_enabled));
    xr_object_instance_set_by_key(X, tls, "clientReady", xr_bool(c->tls_client_ctx != NULL));
    xr_object_instance_set_by_key(X, tls, "serverReady", xr_bool(c->tls_server_ctx != NULL));
    xr_object_instance_set_by_key(X, info, "tls", xr_object_instance_value(tls));

    return xr_object_instance_value(info);
}

// Extended cluster.monitor: 1 arg = node monitor, 2 args = remote coro monitor
static XrValue cluster_monitor_coro_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    if (argc == 1) {
        // Node-level monitor: cluster.monitor("node_name")
        XrString *name_str = XR_TO_STRING(args[0]);
        XrChannel *ch = cluster_monitor_node(X, name_str->data);
        if (!ch)
            return xr_null();
        return xr_value_from_channel(ch);
    }

    // Remote coroutine monitor: cluster.monitor("node_name", "coro_name")
    if (!XR_IS_STRING(args[1]))
        return xr_null();
    XrString *node_str = XR_TO_STRING(args[0]);
    XrString *coro_str = XR_TO_STRING(args[1]);

    XrChannel *ch = cluster_monitor_coro(X, node_str->data, coro_str->data);
    if (!ch)
        return xr_null();
    return xr_value_from_channel(ch);
}

#define XR_STDLIB_VM_BIND_MODULE_CLUSTER 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CLUSTER

/* ========== Module Registration ========== */

