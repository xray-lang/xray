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
#include "cluster_channel.h"
#include "cluster_serial.h"
#include "../common.h"
#include "../crypto/crypto.h"  // xr_secure_wipe
#include "../net/io.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoro_registry.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_internal.h"
#include "../../src/base/xhash.h"
#include "../../src/base/xchecks.h"
#include "../../src/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// xr_coro_create_native is not declared in any public header
extern struct XrCoroutine *xr_coro_create_native(struct XrayIsolate *X, void (*func)(void *),
                                                 void *arg, const char *name);

/*
 * xr_socket_read / xr_socket_set_read_timeout live in
 * src/coro/xsocket.h. We cannot pull that header in here because it
 * includes include/xray_platform.h which defines
 * `static inline void xr_random_bytes(...)`, and stdlib/crypto/crypto.h
 * (already included above for xr_secure_wipe) exposes
 * `int xr_random_bytes(...)` — the two cannot coexist. Forward-declare
 * just the two entry points we need; the real signatures are checked
 * at link time against the canonical definitions in xsocket.c.
 */
extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern void xr_socket_set_read_timeout(struct XrayIsolate *X, int fd, int timeout_ms);

static inline uint32_t str_hash(const char *s) {
    return xr_hash_bytes(s, strlen(s));
}

/* ========== Interruptible Sleep Helper ========== */

/*
 * Block the current coroutine for up to `ms` milliseconds, returning
 * early (false) as soon as xr_cluster_stop closes the stop_pipe.
 *
 * Strategy:
 *   1. Program a read deadline on stop_pipe[0] via the netpoll timer
 *      wheel — same machinery that drives socket read timeouts.
 *   2. Call xr_socket_read on stop_pipe[0]. The coroutine yields
 *      until:
 *        a. deadline expires   → read returns -1; sleep elapsed.
 *        b. stop_pipe[1] closes → read returns 0 (EOF); shutdown.
 *        c. targeted wake byte → read returns > 0; shutdown.
 *   3. Clear the deadline so the next sleep sees a fresh deadline.
 *
 * Requires: stop_pipe created in start_ex (now fatal if pipe() fails)
 * and c->isolate bound (always true for cluster coroutines).
 */
bool xr_cluster_sleep_interruptible(XrCluster *c, int ms) {
    if (!c)
        return false;
    if (!atomic_load(&c->running))
        return false;
    if (ms <= 0)
        return atomic_load(&c->running);

    XR_DCHECK(c->stop_pipe[0] >= 0, "cluster: stop_pipe required");
    XR_DCHECK(c->isolate != NULL, "cluster: isolate required for sleep");

    int rfd = c->stop_pipe[0];
    xr_socket_set_read_timeout(c->isolate, rfd, ms);
    char byte;
    int n = xr_socket_read(c->isolate, rfd, &byte, 1);
    xr_socket_set_read_timeout(c->isolate, rfd, 0);

    if (n == 0)
        return false;  // EOF — stop closed write end
    if (n > 0)
        return false;  // targeted wake (treat as stop)
    return atomic_load(&c->running);
}

/* ========== Heartbeat Coroutine ========== */

/*
 * Drive xr_cluster_send_heartbeats + xr_cluster_check_heartbeats at a
 * steady cadence. Runs as a native coroutine on the normal worker
 * pool so the cluster stays on one scheduling model end-to-end — no
 * more stray pthread with its own sleep granularity.
 *
 * Ticks at heartbeat_interval_ms / 2 (capped at 500ms min) so that
 * phi-accrual has at least two samples per interval and stop-latency
 * is bounded.
 */
static void cluster_heartbeat_coro(void *arg) {
    XrCluster *c = (XrCluster *) arg;
    if (!c)
        return;

    xr_io_set_isolate(c->isolate);
    atomic_store(&c->heartbeat_running, true);

    /* Use half the heartbeat interval for check frequency.
     * This ensures we detect dead nodes within ~1.5x the interval. */
    int sleep_ms = c->heartbeat_interval_ms / 2;
    if (sleep_ms < 500)
        sleep_ms = 500;

    while (atomic_load(&c->running)) {
        if (!xr_cluster_sleep_interruptible(c, sleep_ms))
            break;

        xr_cluster_send_heartbeats(c);
        xr_cluster_check_heartbeats(c);
    }

    atomic_store(&c->heartbeat_running, false);
}

/* ========== Accept Loop ==========
 *
 * Runs as a native coroutine spawned from xr_cluster_start_ex. Handles
 * every inbound peer: coroutine-friendly accept, optional TLS wrap,
 * cluster handshake, then spawns writer+reader coroutines for the new
 * node. Terminates when the listen fd is closed by xr_cluster_stop.
 *
 * The loop is intentionally forgiving of per-connection failures —
 * one bad peer (handshake timeout, wrong secret, expired cert) must
 * not take down the whole accept path. Only a fd-level failure
 * (EBADF from a closed listen_fd) exits the loop.
 */
static void cluster_accept_loop(void *arg) {
    XrCluster *c = (XrCluster *) arg;
    if (!c) {
        return;
    }

    /* Bind the worker thread's tls_isolate so xr_io_accept /
     * xr_io_read / xr_io_write inside this coroutine can resolve a
     * runtime to yield against. Same pattern as stdlib/ws uses for
     * its upgrade coroutine. */
    xr_io_set_isolate(c->isolate);

    atomic_store(&c->accept_running, true);

    while (atomic_load(&c->running)) {
        XrIOConn *conn = NULL;

        if (c->tls_enabled && c->tls_server_ctx) {
            conn = xr_io_accept_tls_with_ctx(c->listen_fd, c->tls_server_ctx);
        } else if (c->tls_enabled && !c->tls_server_ctx) {
            /* Operator asked for TLS but never supplied a cert+key. A
             * silent plaintext fallback would be a loud security hole;
             * refuse the whole accept path instead. The startup logs
             * already surfaced this via build_cluster_tls. */
            break;
        } else {
            conn = xr_io_accept(c->listen_fd);
        }

        if (!conn) {
            /* accept() failure can mean (a) listen fd closed by stop
             * (clean exit) or (b) transient errno=EMFILE / ECONNABORTED.
             * The running flag distinguishes the two — if we are still
             * running, back off briefly so we do not hotloop on
             * EMFILE (the kernel keeps POLLIN armed because a socket
             * is still pending, so a plain retry would spin).
             *
             * We yield via xr_cluster_sleep_interruptible instead of
             * nanosleep so that a concurrent xr_cluster_stop() wakes
             * us immediately rather than after the full 10ms, and —
             * more importantly — so we never pin the worker thread. */
            if (!atomic_load(&c->running))
                break;
            if (!xr_cluster_sleep_interruptible(c, 10))
                break;
            continue;
        }

        /* Server-side handshake. On failure the conn is closed and no
         * XrClusterNode is created — we just drop the peer. */
        XrClusterNode *node = xr_cluster_node_accept(c, conn);
        if (!node) {
            xr_io_close(conn);
            continue;
        }

        /* Tombstone check: reject nodes that were recently dead. They
         * must wait out the tombstone window before rejoining. */
        if (xr_cluster_is_dead(c, node->name)) {
            xr_cluster_node_free(node);
            continue;
        }

        xr_cluster_add_node(c, node);
        xr_cluster_node_start_writer(node, c->isolate);
        xr_cluster_node_start_reader(c, node);
        if (c->on_node_added)
            c->on_node_added(node->name);
    }

    atomic_store(&c->accept_running, false);
}

/* ========== Cluster Lifecycle ========== */

int xr_cluster_start(XrayIsolate *X, const char *name, uint16_t port, const char *secret) {
    // Legacy entry point: plain TCP, no TLS. Forwards to the extended
    // implementation so both paths share one code flow.
    return xr_cluster_start_ex(X, name, port, secret, NULL);
}

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
    // Client context covers outgoing xr_cluster_join / reconnect traffic.
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

int xr_cluster_start_ex(XrayIsolate *X, const char *name, uint16_t port, const char *secret,
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
    xr_amutex_init(&c->channels_lock);
    xr_amutex_init(&c->services_lock);
    xr_amutex_init(&c->dead_nodes_lock);
    xr_amutex_init(&c->topics_lock);
    atomic_store(&c->next_request_id, 1);

    /* Topic routing trie — allocated eagerly so subscribe never has to
     * worry about a NULL root under the lock. Failure here is fatal to
     * start: pub/sub is a first-class feature, not a best-effort add-on. */
    if (xr_cluster_topics_init(c) != 0) {
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
    c->max_pending_requests = XR_MAX_PENDING_REQUESTS;
    // Dynamic tombstone array
    c->tombstone_cap = 16;
    c->tombstones = xr_calloc((size_t) c->tombstone_cap, sizeof(c->tombstones[0]));
    c->tombstone_count = 0;
    c->on_node_added = NULL;
    c->on_node_removed = NULL;
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

    /*
     * Stop-signalling pipe. Required for coroutine-friendly
     * interruptible sleep. Failure is fatal — without it every
     * sleep in the cluster degrades to nanosleep, blocking the
     * worker thread and starving other coroutines.
     */
    c->stop_pipe[0] = -1;
    c->stop_pipe[1] = -1;
    if (pipe(c->stop_pipe) != 0) {
        close(c->listen_fd);
        if (c->tls_client_ctx)
            xr_tls_context_free(c->tls_client_ctx);
        if (c->tls_server_ctx)
            xr_tls_context_free(c->tls_server_ctx);
        xr_secure_wipe(c->secret, sizeof(c->secret));
        xr_free(c);
        return -1;
    }
    fcntl(c->stop_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(c->stop_pipe[1], F_SETFL, O_NONBLOCK);

    X->cluster = c;
    xr_cluster_channel_install_hooks(X);

    /*
     * Spawn the heartbeat coroutine. It drives send_heartbeats +
     * check_heartbeats and now lives on the normal worker pool
     * instead of a dedicated pthread — same cadence, same work, but
     * shares scheduling and liveness tracking with the rest of the
     * cluster machinery.
     */
    XrCoroutine *hb_coro = xr_coro_create_native(X, cluster_heartbeat_coro, c, "cluster_heartbeat");
    if (hb_coro) {
        xr_coro_spawn(X, hb_coro);
        c->heartbeat_coro_spawned = true;
    }

    /*
     * Spawn the inbound-accept coroutine. Failure here is non-fatal
     * for outbound-only deployments (think: edge nodes that only
     * initiate to a central core), but we still surface it via the
     * accept_coro_spawned flag so xr_cluster_stop does not wait for
     * something that never ran.
     */
    XrCoroutine *accept_coro = xr_coro_create_native(X, cluster_accept_loop, c, "cluster_accept");
    if (accept_coro) {
        xr_coro_spawn(X, accept_coro);
        c->accept_coro_spawned = true;
    }

    return 0;
}

void xr_cluster_stop(XrCluster *c) {
    if (!c)
        return;

    atomic_store(&c->running, false);

    /*
     * Close the write end of stop_pipe first. Every coroutine inside
     * xr_cluster_sleep_interruptible is yielded on a read(2) against
     * stop_pipe[0]; EOF wakes them immediately regardless of how far
     * into a deadline they had gotten. We leave the read end open
     * until after every user has observed EOF so no one hits a
     * half-closed EBADF race.
     */
    if (c->stop_pipe[1] >= 0) {
        close(c->stop_pipe[1]);
        c->stop_pipe[1] = -1;
    }

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

    /*
     * Wait (bounded) for the accept coroutine to exit before tearing
     * down cluster state it still references. 1s is enough for the
     * netpoll wake + a single loop iteration; if we miss the window
     * we proceed anyway — the coro checks c->running at the top and
     * we already closed listen_fd, so at worst it observes a stale
     * isolate pointer and a failed accept and bails out.
     *
     * We spin with nanosleep (not xr_cluster_sleep_interruptible)
     * because stop() runs on the embedder thread, not a coroutine.
     * Bounded to 1s total; coroutines typically exit within one tick.
     */
    if (c->accept_coro_spawned) {
        for (int i = 0; i < 100 && atomic_load(&c->accept_running); i++) {
            xr_time_sleep_ns(10ULL * 1000ULL * 1000ULL);
        }
        c->accept_coro_spawned = false;
    }

    /*
     * Same bounded wait for the heartbeat coroutine. Its teardown
     * order matters: send_heartbeats touches c->nodes, so we must let
     * the coro exit before the nodes_lock / nodes sweep below.
     */
    if (c->heartbeat_coro_spawned) {
        for (int i = 0; i < 100 && atomic_load(&c->heartbeat_running); i++) {
            xr_time_sleep_ns(10ULL * 1000ULL * 1000ULL);
        }
        c->heartbeat_coro_spawned = false;
    }

    // Stop LAN discovery thread (checks c->running)
    xr_cluster_discovery_stop(c);

    // Uninstall distributed channel hooks (per-isolate)
    xr_cluster_channel_uninstall_hooks(c->isolate);

    // Close all node connections
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        XrClusterNode *next = node->next;
        xr_cluster_node_free(node);
        node = next;
    }
    c->nodes = NULL;
    c->node_count = 0;
    xr_amutex_unlock(&c->nodes_lock);

    // Free channel registry
    xr_amutex_lock(&c->channels_lock);
    for (int i = 0; i < XR_CLUSTER_CHANNEL_BUCKETS; i++) {
        XrDistChannel *dc = c->channel_buckets[i];
        while (dc) {
            XrDistChannel *next = dc->next;
            xr_free(dc);
            dc = next;
        }
        c->channel_buckets[i] = NULL;
    }
    c->channel_count = 0;
    xr_amutex_unlock(&c->channels_lock);

    // Free service registry
    xr_amutex_lock(&c->services_lock);
    for (int i = 0; i < XR_CLUSTER_SERVICE_BUCKETS; i++) {
        XrServiceEntry *se = c->service_buckets[i];
        while (se) {
            XrServiceEntry *next = se->next;
            xr_free(se);
            se = next;
        }
        c->service_buckets[i] = NULL;
    }
    c->service_count = 0;
    xr_amutex_unlock(&c->services_lock);

    // Free topic subscriptions (recursive trie teardown lives in cluster_topic.c)
    xr_cluster_topics_destroy(c);

    // Free node monitors
    xr_amutex_lock(&c->monitors_lock);
    {
        XrNodeMonitor *mon = c->monitors;
        while (mon) {
            XrNodeMonitor *next = mon->next;
            xr_free(mon);
            mon = next;
        }
        c->monitors = NULL;
        c->monitor_count = 0;
    }
    xr_amutex_unlock(&c->monitors_lock);

    // Free remote coroutine monitors
    {
        XrRemoteCoroMonitor *rm = c->remote_coro_monitors;
        while (rm) {
            XrRemoteCoroMonitor *next = rm->next;
            xr_free(rm);
            rm = next;
        }
        c->remote_coro_monitors = NULL;
    }

    // Free dynamic tombstones
    xr_free(c->tombstones);
    c->tombstones = NULL;

    // Release TLS contexts built in xr_cluster_start_ex. Freeing happens
    // after xr_cluster_node_free loops so no node writer coroutine can
    // still be dereferencing conn->tls which points into these contexts.
    if (c->tls_client_ctx) {
        xr_tls_context_free(c->tls_client_ctx);
        c->tls_client_ctx = NULL;
    }
    if (c->tls_server_ctx) {
        xr_tls_context_free(c->tls_server_ctx);
        c->tls_server_ctx = NULL;
    }
    c->tls_enabled = false;

    // Scrub the shared secret before freeing the struct.
    xr_secure_wipe(c->secret, sizeof(c->secret));

    /*
     * Close the read end of stop_pipe last — after every user
     * (heartbeat coro, accept coro, discovery coro, reconnect helper)
     * has exited. Doing it earlier would risk a use-after-close on a
     * coroutine that had not yet observed EOF on pipe[1].
     */
    if (c->stop_pipe[0] >= 0) {
        close(c->stop_pipe[0]);
        c->stop_pipe[0] = -1;
    }

    if (c->isolate)
        c->isolate->cluster = NULL;
    xr_free(c);
}

bool xr_cluster_is_running(XrCluster *c) {
    return c && atomic_load(&c->running);
}

const char *xr_cluster_self_name(XrCluster *c) {
    return c ? c->self_name : "";
}

/* ========== Node Management ========== */

XrClusterNode *xr_cluster_find_node(XrCluster *c, const char *name) {
    if (!c)
        return NULL;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            xr_amutex_unlock(&c->nodes_lock);
            return node;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    return NULL;
}

void xr_cluster_add_node(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return;

    xr_amutex_lock(&c->nodes_lock);
    node->next = c->nodes;
    c->nodes = node;
    c->node_count++;
    xr_amutex_unlock(&c->nodes_lock);
}

void xr_cluster_remove_node(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return;

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode **pp = &c->nodes;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next;
            c->node_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
}

int xr_cluster_join(XrCluster *c, const char *host, uint16_t port) {
    if (!c)
        return -1;

    XrClusterNode *node = xr_cluster_node_new(NULL, host, port);
    if (!node)
        return -1;

    if (xr_cluster_node_connect(c, node) != 0) {
        xr_cluster_node_free(node);
        return -1;
    }

    xr_cluster_add_node(c, node);

    /* Spawn the async writer AND the frame-processing reader. Both are
     * required for a bidirectional link — pre-P14 the reader was never
     * started, which meant inbound RPC responses and heartbeats went
     * unnoticed and the peer was torn down by the phi detector within
     * two heartbeat intervals. */
    xr_cluster_node_start_writer(node, c->isolate);
    xr_cluster_node_start_reader(c, node);

    return 0;
}

/* ========== Named Channel Registry ========== */

void xr_cluster_register_channel(XrCluster *c, const char *name, struct XrChannel *ch) {
    if (!c || !name || !ch)
        return;

    XrDistChannel *dc = (XrDistChannel *) xr_calloc(1, sizeof(XrDistChannel));
    if (!dc)
        return;

    strncpy(dc->name, name, XR_CHANNEL_NAME_MAX);
    dc->name[XR_CHANNEL_NAME_MAX] = '\0';
    dc->is_owner = true;
    dc->owner_node = NULL;
    dc->channel = ch;
    dc->cluster = c;

    // Set channel's name and dist pointer
    ch->name = dc->name;
    ch->dist = dc;

    uint32_t bucket = str_hash(name) % XR_CLUSTER_CHANNEL_BUCKETS;

    xr_amutex_lock(&c->channels_lock);
    dc->next = c->channel_buckets[bucket];
    c->channel_buckets[bucket] = dc;
    c->channel_count++;
    xr_amutex_unlock(&c->channels_lock);
}

XrDistChannel *xr_cluster_find_channel(XrCluster *c, const char *name) {
    if (!c || !name)
        return NULL;

    uint32_t bucket = str_hash(name) % XR_CLUSTER_CHANNEL_BUCKETS;

    xr_amutex_lock(&c->channels_lock);
    XrDistChannel *dc = c->channel_buckets[bucket];
    while (dc) {
        if (strcmp(dc->name, name) == 0) {
            xr_amutex_unlock(&c->channels_lock);
            return dc;
        }
        dc = dc->next;
    }
    xr_amutex_unlock(&c->channels_lock);
    return NULL;
}

void xr_cluster_unregister_channel(XrCluster *c, const char *name) {
    if (!c || !name)
        return;

    uint32_t bucket = str_hash(name) % XR_CLUSTER_CHANNEL_BUCKETS;

    xr_amutex_lock(&c->channels_lock);
    XrDistChannel **pp = &c->channel_buckets[bucket];
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            XrDistChannel *dc = *pp;
            *pp = dc->next;
            c->channel_count--;
            // Clear channel's dist pointer
            if (dc->channel) {
                dc->channel->dist = NULL;
                dc->channel->name = NULL;
            }
            xr_free(dc);
            break;
        }
        pp = &(*pp)->next;
    }
    xr_amutex_unlock(&c->channels_lock);
}

/* ========== Service Registry ========== */

XrChannel *xr_cluster_register_service(XrayIsolate *X, const char *name) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !name)
        return NULL;

    XrServiceEntry *se = (XrServiceEntry *) xr_calloc(1, sizeof(XrServiceEntry));
    if (!se)
        return NULL;

    strncpy(se->name, name, XR_SERVICE_NAME_MAX);
    se->name[XR_SERVICE_NAME_MAX] = '\0';

    // Create a buffered channel for incoming requests
    se->request_ch = xr_channel_new(X, 64);
    if (!se->request_ch) {
        xr_free(se);
        return NULL;
    }

    uint32_t bucket = str_hash(name) % XR_CLUSTER_SERVICE_BUCKETS;

    xr_amutex_lock(&c->services_lock);
    se->next = c->service_buckets[bucket];
    c->service_buckets[bucket] = se;
    c->service_count++;
    xr_amutex_unlock(&c->services_lock);

    return se->request_ch;
}

XrServiceEntry *xr_cluster_find_service(XrCluster *c, const char *name) {
    if (!c || !name)
        return NULL;

    uint32_t bucket = str_hash(name) % XR_CLUSTER_SERVICE_BUCKETS;

    xr_amutex_lock(&c->services_lock);
    XrServiceEntry *se = c->service_buckets[bucket];
    while (se) {
        if (strcmp(se->name, name) == 0) {
            xr_amutex_unlock(&c->services_lock);
            return se;
        }
        se = se->next;
    }
    xr_amutex_unlock(&c->services_lock);
    return NULL;
}

/* ========== Subscriber Management (for select push model) ========== */

void xr_cluster_add_subscriber(XrCluster *c, const char *channel_name, XrClusterNode *node) {
    if (!c || !channel_name || !node)
        return;

    xr_amutex_lock(&c->channels_lock);
    uint32_t bucket = str_hash(channel_name) % XR_CLUSTER_CHANNEL_BUCKETS;
    XrDistChannel *dc = c->channel_buckets[bucket];
    while (dc) {
        if (strcmp(dc->name, channel_name) == 0 && dc->is_owner) {
            XrChannelSubscribers *subs = &dc->subscribers;
            // Check for duplicate
            for (int i = 0; i < subs->count; i++) {
                if (subs->nodes[i] == node) {
                    xr_amutex_unlock(&c->channels_lock);
                    return;  // already subscribed
                }
            }
            // Add new subscriber if room
            if (subs->count < XR_MAX_SUBSCRIBERS) {
                subs->nodes[subs->count++] = node;
            }
            break;
        }
        dc = dc->next;
    }
    xr_amutex_unlock(&c->channels_lock);
}

void xr_cluster_remove_subscriber(XrCluster *c, const char *channel_name, XrClusterNode *node) {
    if (!c || !channel_name || !node)
        return;

    xr_amutex_lock(&c->channels_lock);
    uint32_t bucket = str_hash(channel_name) % XR_CLUSTER_CHANNEL_BUCKETS;
    XrDistChannel *dc = c->channel_buckets[bucket];
    while (dc) {
        if (strcmp(dc->name, channel_name) == 0 && dc->is_owner) {
            XrChannelSubscribers *subs = &dc->subscribers;
            for (int i = 0; i < subs->count; i++) {
                if (subs->nodes[i] == node) {
                    // Swap with last element for O(1) removal
                    subs->nodes[i] = subs->nodes[subs->count - 1];
                    subs->count--;
                    break;
                }
            }
            break;
        }
        dc = dc->next;
    }
    xr_amutex_unlock(&c->channels_lock);
}

void xr_cluster_remove_all_subscribers_for_node(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return;

    xr_amutex_lock(&c->channels_lock);
    for (int i = 0; i < XR_CLUSTER_CHANNEL_BUCKETS; i++) {
        XrDistChannel *dc = c->channel_buckets[i];
        while (dc) {
            if (dc->is_owner) {
                XrChannelSubscribers *subs = &dc->subscribers;
                for (int j = 0; j < subs->count;) {
                    if (subs->nodes[j] == node) {
                        subs->nodes[j] = subs->nodes[subs->count - 1];
                        subs->count--;
                    } else {
                        j++;
                    }
                }
            }
            dc = dc->next;
        }
    }
    xr_amutex_unlock(&c->channels_lock);
}

/* ========== xray Function Bindings ========== */

// cluster.start(config) - config is Json with {name, port, secret, tls?}
//
// The optional `tls` sub-object maps 1:1 onto XrClusterTlsOptions:
//     tls: {
//         enabled: true,
//         caFile:   "/etc/xray/ca.pem",
//         certFile: "/etc/xray/node.crt",
//         keyFile:  "/etc/xray/node.key",
//         insecure: false
//     }
// Missing keys fall back to the struct's zero-initialised defaults (off /
// NULL). The strings stay borrowed from the Json for the duration of this
// call — cluster_start_ex copies them into OpenSSL contexts before it
// returns, so no lifetime surprise.
static XrValue cluster_start(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_JSON(args[0]))
        return xr_null();

    XrJson *config = (XrJson *) XR_TO_PTR(args[0]);
    XrValue v_name = xr_json_get_by_key(X, config, "name");
    XrValue v_port = xr_json_get_by_key(X, config, "port");
    XrValue v_secret = xr_json_get_by_key(X, config, "secret");

    if (!XR_IS_STRING(v_name) || !XR_IS_INT(v_port))
        return xr_null();

    XrString *name = XR_TO_STRING(v_name);
    uint16_t port = (uint16_t) XR_TO_INT(v_port);
    const char *secret = "";
    if (XR_IS_STRING(v_secret)) {
        secret = XR_TO_STRING(v_secret)->data;
    }

    // Optional TLS block. Absent or non-object → plain TCP (legacy path).
    XrClusterTlsOptions tls_opts;
    memset(&tls_opts, 0, sizeof(tls_opts));
    const XrClusterTlsOptions *tls_ptr = NULL;

    XrValue v_tls = xr_json_get_by_key(X, config, "tls");
    if (XR_IS_JSON(v_tls)) {
        XrJson *tls_cfg = (XrJson *) XR_TO_PTR(v_tls);

        XrValue v_enabled = xr_json_get_by_key(X, tls_cfg, "enabled");
        if (XR_IS_BOOL(v_enabled)) {
            tls_opts.enabled = XR_TO_BOOL(v_enabled);
        } else {
            // Treat a bare `tls: {...}` with no explicit `enabled` as on;
            // operators who went to the trouble of populating the block
            // almost always mean "use it".
            tls_opts.enabled = true;
        }

        XrValue v_ca = xr_json_get_by_key(X, tls_cfg, "caFile");
        XrValue v_cert = xr_json_get_by_key(X, tls_cfg, "certFile");
        XrValue v_key = xr_json_get_by_key(X, tls_cfg, "keyFile");
        XrValue v_ins = xr_json_get_by_key(X, tls_cfg, "insecure");

        if (XR_IS_STRING(v_ca))
            tls_opts.ca_file = XR_TO_STRING(v_ca)->data;
        if (XR_IS_STRING(v_cert))
            tls_opts.cert_file = XR_TO_STRING(v_cert)->data;
        if (XR_IS_STRING(v_key))
            tls_opts.key_file = XR_TO_STRING(v_key)->data;
        if (XR_IS_BOOL(v_ins))
            tls_opts.insecure = XR_TO_BOOL(v_ins);

        tls_ptr = &tls_opts;
    }

    int rc = xr_cluster_start_ex(X, name->data, port, secret, tls_ptr);
    return xr_bool(rc == 0);
}

// cluster.join(addr) - addr is "host:port" string
static XrValue cluster_join(XrayIsolate *X, XrValue *args, int argc) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);

    XrString *addr = XR_TO_STRING(args[0]);
    char host[256] = {0};
    uint16_t port = 0;

    // Parse "host:port"
    const char *colon = strrchr(addr->data, ':');
    if (!colon)
        return xr_bool(0);

    size_t host_len = (size_t) (colon - addr->data);
    if (host_len >= sizeof(host))
        return xr_bool(0);
    memcpy(host, addr->data, host_len);
    host[host_len] = '\0';
    port = (uint16_t) atoi(colon + 1);

    int rc = xr_cluster_join(c, host, port);
    return xr_bool(rc == 0);
}

// cluster.self() - returns node name
static XrValue cluster_self(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    const char *name = xr_cluster_self_name(c);
    XrString *str = xr_string_intern(X, name, (uint32_t) strlen(name), 0);
    return xr_string_value(str);
}

// cluster.nodes() - returns array of connected node names
static XrValue cluster_nodes(XrayIsolate *X, XrValue *args, int argc) {
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

// cluster.channel(name, size) - create or get Named Channel
static XrValue cluster_channel_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *name_str = XR_TO_STRING(args[0]);
    uint32_t buf_size = 0;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        buf_size = (uint32_t) XR_TO_INT(args[1]);
    }

    XrCluster *c = (XrCluster *) X->cluster;

    // If cluster running, check for existing channel (e.g. Proxy from CHANNEL_SYNC)
    if (xr_cluster_is_running(c)) {
        XrDistChannel *existing = xr_cluster_find_channel(c, name_str->data);
        if (existing && existing->channel) {
            return xr_value_from_channel(existing->channel);
        }
    }

    XrChannel *ch = xr_channel_new(X, buf_size);
    if (!ch)
        return xr_null();

    if (xr_cluster_is_running(c)) {
        xr_cluster_register_channel(c, name_str->data, ch);
    }

    return xr_value_from_channel(ch);
}

// cluster.serve(name) - register service + return request Channel
static XrValue cluster_serve_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *name_str = XR_TO_STRING(args[0]);
    XrChannel *ch = xr_cluster_register_service(X, name_str->data);
    if (!ch)
        return xr_null();

    return xr_value_from_channel(ch);
}

// cluster.reply(req, result) - simplified: auto-extract id/from from req Json
// Also supports legacy: cluster.reply(id, from, result)
static XrValue cluster_reply_fn(XrayIsolate *X, XrValue *args, int argc) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_bool(0);

    uint64_t request_id;
    const char *from_name;
    XrValue result;

    if (argc >= 2 && XR_IS_JSON(args[0])) {
        // Simplified form: cluster.reply(req, result)
        XrJson *req = (XrJson *) XR_TO_PTR(args[0]);
        result = args[1];

        // Check for local reply_ch first (local service call path)
        XrValue reply_ch_val = xr_json_get_by_key(X, req, "reply_ch");
        if (XR_IS_CHANNEL(reply_ch_val)) {
            XrChannel *reply_ch = (XrChannel *) XR_TO_PTR(reply_ch_val);
            xr_channel_try_send(reply_ch, result);
            return xr_bool(1);
        }

        // Remote reply: extract id and from
        XrValue v_id = xr_json_get_by_key(X, req, "id");
        XrValue v_from = xr_json_get_by_key(X, req, "from");
        if (!XR_IS_INT(v_id) || !XR_IS_STRING(v_from))
            return xr_bool(0);
        request_id = (uint64_t) XR_TO_INT(v_id);
        from_name = XR_TO_STRING(v_from)->data;
    } else if (argc >= 3 && XR_IS_INT(args[0]) && XR_IS_STRING(args[1])) {
        // Legacy form: cluster.reply(id, from, result)
        request_id = (uint64_t) XR_TO_INT(args[0]);
        from_name = XR_TO_STRING(args[1])->data;
        result = args[2];
    } else {
        return xr_bool(0);
    }

    // Find the requesting node
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *target = NULL;
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, from_name) == 0 && node->state == XR_NODE_CONNECTED) {
            target = node;
            break;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    if (!target)
        return xr_bool(0);

    // Serialize result
    XrSerialBuf sbuf;
    xr_serial_buf_init(&sbuf);
    if (xr_cluster_encode(X, result, &sbuf) != 0) {
        xr_serial_buf_free(&sbuf);
        return xr_bool(0);
    }

    // Encode and enqueue SERVICE_REPLY frame via output queue
    size_t frame_size = 4 + 1 + 8 + 1 + sbuf.len;
    uint8_t stack_frame[4096];
    uint8_t *frame = (frame_size + 16 <= sizeof(stack_frame))
                         ? stack_frame
                         : (uint8_t *) xr_malloc(frame_size + 16);
    if (!frame) {
        xr_serial_buf_free(&sbuf);
        return xr_bool(0);
    }

    int flen = xr_frame_encode_service_reply(frame, frame_size + 16, request_id, false, sbuf.data,
                                             (uint32_t) sbuf.len);
    xr_serial_buf_free(&sbuf);

    if (flen < 0) {
        if (frame != stack_frame)
            xr_free(frame);
        return xr_bool(0);
    }

    int rc = xr_cluster_node_enqueue(target, frame, (uint32_t) flen);
    if (frame != stack_frame)
        xr_free(frame);
    return xr_bool(rc == 0);
}

// cluster.call(name, args, timeout) - remote service call
// Uses pending request table: sends SERVICE_CALL, then blocks on a temp Channel
// that process_node will deliver the result to. No direct socket read.
static XrValue cluster_call_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrString *service_name = XR_TO_STRING(args[0]);
    XrValue call_args = args[1];
    (void) argc;  // timeout not yet used for pending approach

    // Check if service is local
    XrServiceEntry *se = xr_cluster_find_service(c, service_name->data);
    if (se && se->request_ch) {
        // Local service: directly send to request channel
        uint64_t req_id = atomic_fetch_add(&c->next_request_id, 1);
        XrChannel *rsp_ch = xr_channel_new(X, 1);
        if (!rsp_ch)
            return xr_null();

        XrJson *req_json = xr_json_new(NULL, 4);
        if (!req_json)
            return xr_null();
        xr_json_set_by_key(X, req_json, "id", xr_int((int64_t) req_id));
        xr_json_set_by_key(
            X, req_json, "from",
            xr_string_value(xr_string_intern(X, c->self_name, (uint32_t) strlen(c->self_name), 0)));
        xr_json_set_by_key(X, req_json, "args", call_args);
        xr_json_set_by_key(X, req_json, "reply_ch", xr_value_from_channel(rsp_ch));
        xr_channel_try_send(se->request_ch, xr_json_value(req_json));

        // Block waiting for local reply
        bool ok = false;
        XrValue result = xr_channel_try_recv(rsp_ch, &ok);
        return ok ? result : xr_null();
    }

    // Remote service: serialize args
    XrSerialBuf sbuf;
    xr_serial_buf_init(&sbuf);
    if (xr_cluster_encode(X, call_args, &sbuf) != 0) {
        xr_serial_buf_free(&sbuf);
        return xr_null();
    }

    uint64_t req_id = atomic_fetch_add(&c->next_request_id, 1);

    // Route to first connected node
    XrClusterNode *target = NULL;
    xr_amutex_lock(&c->nodes_lock);
    target = c->nodes;
    while (target && target->state != XR_NODE_CONNECTED) {
        target = target->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    if (!target || !target->conn) {
        xr_serial_buf_free(&sbuf);
        return xr_null();
    }

    // Register pending request BEFORE sending (avoid race)
    XrChannel *rsp_ch = xr_cluster_node_add_pending(target, req_id, X, c->max_pending_requests);
    if (!rsp_ch) {
        xr_serial_buf_free(&sbuf);
        return xr_null();
    }

    // Build and send SERVICE_CALL frame
    size_t frame_size = 4 + 1 + 8 + 1 + strlen(service_name->data) + sbuf.len;
    uint8_t stack_frame[4096];
    uint8_t *frame = (frame_size + 16 <= sizeof(stack_frame))
                         ? stack_frame
                         : (uint8_t *) xr_malloc(frame_size + 16);
    if (!frame) {
        xr_serial_buf_free(&sbuf);
        return xr_null();
    }

    int flen = xr_frame_encode_service_call(frame, frame_size + 16, req_id, service_name->data,
                                            sbuf.data, (uint32_t) sbuf.len);
    xr_serial_buf_free(&sbuf);

    if (flen < 0) {
        if (frame != stack_frame)
            xr_free(frame);
        xr_cluster_node_take_pending(target, req_id);  // cleanup
        return xr_null();
    }

    int rc = xr_cluster_node_enqueue(target, frame, (uint32_t) flen);
    if (frame != stack_frame)
        xr_free(frame);
    if (rc != 0) {
        xr_cluster_node_take_pending(target, req_id);
        return xr_null();
    }

    // Block on temp Channel — process_node will deliver the result
    bool ok = false;
    XrValue result = xr_channel_try_recv(rsp_ch, &ok);
    return ok ? result : xr_null();
}

// cluster.monitor(node_name) - returns Channel that receives notification on disconnect
// Use "*" to monitor all nodes
static __attribute__((unused)) XrValue cluster_monitor_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *name_str = XR_TO_STRING(args[0]);
    XrChannel *ch = xr_cluster_monitor_node(X, name_str->data);
    if (!ch)
        return xr_null();

    return xr_value_from_channel(ch);
}

// cluster.discover() - start LAN auto-discovery via UDP multicast
static XrValue cluster_discover_fn(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_bool(0);

    int rc = xr_cluster_discovery_start(c);
    return xr_bool(rc == 0);
}

// cluster.stop()
static XrValue cluster_stop_fn(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    xr_cluster_stop((XrCluster *) X->cluster);
    return xr_null();
}

/* ========== Frame Processing Loop ========== */

void xr_cluster_process_node(XrCluster *c, XrClusterNode *node) {
    XR_DCHECK(c != NULL, "cluster must be initialized");
    XR_DCHECK(node != NULL, "node must not be NULL");
    if (!c || !node)
        return;

    // Heap-allocated receive buffer (avoid 64KB on coroutine stack)
    uint8_t *recv_buf = (uint8_t *) xr_malloc(65536);
    if (!recv_buf)
        return;
    uint8_t frame_type;
    uint32_t payload_len;

    while (atomic_load(&c->running) && node->state == XR_NODE_CONNECTED) {
        if (xr_cluster_node_recv_frame(node, &frame_type, recv_buf, 65536, &payload_len) != 0) {
            break;  // Disconnect
        }

        switch (frame_type) {
            case XR_FRAME_HEARTBEAT_PING: {
                // Reply with PONG via output queue
                int64_t ts;
                if (xr_frame_decode_heartbeat(recv_buf, payload_len, &ts) == 0) {
                    uint8_t pong[32];
                    int plen =
                        xr_frame_encode_heartbeat(pong, sizeof(pong), XR_FRAME_HEARTBEAT_PONG, ts);
                    if (plen > 0) {
                        xr_cluster_node_enqueue(node, pong, (uint32_t) plen);
                    }
                }
                int64_t now_hb = xr_cluster_now_ms();
                node->last_heartbeat_recv = now_hb;
                node->missed_heartbeats = 0;
                xr_phi_record_heartbeat(&node->phi, now_hb);
                atomic_fetch_add(&node->metrics.frames_recv, 1);
                break;
            }

            case XR_FRAME_HEARTBEAT_PONG: {
                int64_t now_pong = xr_cluster_now_ms();
                // Compute RTT from ping timestamp
                int64_t ping_ts;
                if (xr_frame_decode_heartbeat(recv_buf, payload_len, &ping_ts) == 0) {
                    node->metrics.last_rtt_ms = now_pong - ping_ts;
                }
                node->last_heartbeat_recv = now_pong;
                node->missed_heartbeats = 0;
                xr_phi_record_heartbeat(&node->phi, now_pong);
                atomic_fetch_add(&node->metrics.frames_recv, 1);
                break;
            }

            case XR_FRAME_CHANNEL_SEND: {
                XrFrameChannelSend cs;
                if (xr_frame_decode_channel_send(recv_buf, payload_len, &cs) == 0) {
                    xr_cluster_channel_handle_send(c, cs.channel_name, cs.value_data, cs.value_len);
                }
                break;
            }

            case XR_FRAME_CHANNEL_CLOSE: {
                char ch_name[XR_CHANNEL_NAME_MAX + 1];
                if (xr_frame_decode_channel_close(recv_buf, payload_len, ch_name,
                                                  sizeof(ch_name)) == 0) {
                    xr_cluster_channel_handle_close(c, ch_name);
                }
                break;
            }

            case XR_FRAME_CHANNEL_RECV_RSP: {
                // Format: [request_id 8B] [has_value 1B] [value_data ...]
                if (payload_len < 9)
                    break;
                uint64_t rsp_req_id = 0;
                for (int j = 0; j < 8; j++)
                    rsp_req_id = (rsp_req_id << 8) | recv_buf[j];

                XrChannel *rsp_ch = xr_cluster_node_take_pending(node, rsp_req_id);
                if (!rsp_ch)
                    break;

                if (recv_buf[8] == 0) {
                    // No value — close channel to unblock caller
                    xr_channel_close(rsp_ch);
                } else {
                    XrValue val;
                    if (xr_cluster_decode_value(c->isolate, recv_buf + 9, payload_len - 9, &val) ==
                        0) {
                        xr_channel_try_send(rsp_ch, val);
                    } else {
                        xr_channel_close(rsp_ch);
                    }
                }
                break;
            }

            case XR_FRAME_CHANNEL_RECV_REQ: {
                // Format: [request_id 8B] [name_len 1B] [name ...]
                if (payload_len < 9)
                    break;
                uint64_t recv_req_id = 0;
                for (int j = 0; j < 8; j++)
                    recv_req_id = (recv_req_id << 8) | recv_buf[j];
                uint8_t name_len = recv_buf[8];
                if (name_len > XR_CHANNEL_NAME_MAX || payload_len < (uint32_t) (9 + name_len))
                    break;
                char ch_name[XR_CHANNEL_NAME_MAX + 1];
                memcpy(ch_name, recv_buf + 9, name_len);
                ch_name[name_len] = '\0';

                // Response format: [request_id 8B] [has_value 1B] [value_data ...]
                // Use heap buffer to avoid large stack allocation
                uint8_t *rsp_payload = recv_buf;  // reuse recv_buf (safe: we finished reading)
                for (int j = 7; j >= 0; j--) {
                    rsp_payload[j] = (uint8_t) (recv_req_id & 0xFF);
                    recv_req_id >>= 8;
                }

                XrDistChannel *dc = xr_cluster_find_channel(c, ch_name);
                if (dc && dc->is_owner && dc->channel) {
                    XrValue out;
                    bool ok = false;
                    out = xr_channel_try_recv(dc->channel, &ok);
                    if (ok) {
                        XrSerialBuf sbuf;
                        xr_serial_buf_init(&sbuf);
                        if (xr_cluster_encode(c->isolate, out, &sbuf) == 0) {
                            rsp_payload[8] = 1;  // has_value = true
                            memcpy(rsp_payload + 9, sbuf.data, sbuf.len);
                            xr_cluster_node_send_frame(node, XR_FRAME_CHANNEL_RECV_RSP, rsp_payload,
                                                       9 + (uint32_t) sbuf.len);
                        }
                        xr_serial_buf_free(&sbuf);
                    } else {
                        rsp_payload[8] = 0;  // has_value = false
                        xr_cluster_node_send_frame(node, XR_FRAME_CHANNEL_RECV_RSP, rsp_payload, 9);
                    }
                } else {
                    rsp_payload[8] = 0;
                    xr_cluster_node_send_frame(node, XR_FRAME_CHANNEL_RECV_RSP, rsp_payload, 9);
                }
                break;
            }

            case XR_FRAME_SERVICE_CALL: {
                XrFrameServiceCall sc;
                if (xr_frame_decode_service_call(recv_buf, payload_len, &sc) != 0)
                    break;

                XrServiceEntry *se = xr_cluster_find_service(c, sc.service_name);
                if (!se || !se->request_ch)
                    break;

                // Decode args
                XrValue decoded_args;
                if (xr_cluster_decode_value(c->isolate, sc.args_data, sc.args_len, &decoded_args) !=
                    0)
                    break;

                // Build request Json: {id: int, from: string, args: value}
                XrJson *req_json = xr_json_new(NULL, 3);
                if (req_json) {
                    xr_json_set_by_key(c->isolate, req_json, "id", xr_int((int64_t) sc.request_id));
                    XrString *from_str =
                        xr_string_intern(c->isolate, node->name, (uint32_t) strlen(node->name), 0);
                    xr_json_set_by_key(c->isolate, req_json, "from", xr_string_value(from_str));
                    xr_json_set_by_key(c->isolate, req_json, "args", decoded_args);

                    // Send to service request channel (non-blocking)
                    xr_channel_try_send(se->request_ch, xr_json_value(req_json));
                }
                break;
            }

            case XR_FRAME_SERVICE_REPLY: {
                XrFrameServiceReply reply;
                if (xr_frame_decode_service_reply(recv_buf, payload_len, &reply) != 0)
                    break;

                // Find pending request by request_id
                XrChannel *rsp_ch = xr_cluster_node_take_pending(node, reply.request_id);
                if (!rsp_ch)
                    break;

                if (reply.is_error || reply.result_len == 0) {
                    // Signal error by closing the channel
                    xr_channel_close(rsp_ch);
                } else {
                    // Decode result and deliver to waiting caller
                    XrValue result;
                    if (xr_cluster_decode_value(c->isolate, reply.result_data, reply.result_len,
                                                &result) == 0) {
                        xr_channel_try_send(rsp_ch, result);
                    } else {
                        xr_channel_close(rsp_ch);
                    }
                }
                break;
            }

            case XR_FRAME_NODE_INFO: {
                xr_cluster_handle_node_info(c, recv_buf, payload_len);
                break;
            }

            case XR_FRAME_CHANNEL_SYNC: {
                // Remote node telling us about their Named Channels
                // Parse: [name_len 1B] [name] [owner_len 1B] [owner] [buf_size 4B]
                if (payload_len < 2)
                    break;
                const uint8_t *p = recv_buf;
                uint8_t name_len = *p++;
                if (name_len > XR_CHANNEL_NAME_MAX)
                    break;
                char ch_name[XR_CHANNEL_NAME_MAX + 1];
                memcpy(ch_name, p, name_len);
                ch_name[name_len] = '\0';
                p += name_len;

                // Read buf_size from payload (after owner name)
                uint32_t remote_buf_size = 16;  // default
                if (p < recv_buf + payload_len) {
                    uint8_t owner_len = *p++;
                    p += owner_len;  // skip owner name
                    if (p + 4 <= recv_buf + payload_len) {
                        remote_buf_size = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                                          ((uint32_t) p[2] << 8) | p[3];
                        if (remote_buf_size == 0)
                            remote_buf_size = 16;
                    }
                }

                // Check if we already know this channel
                if (!xr_cluster_find_channel(c, ch_name)) {
                    // Create a proxy channel entry with local buffer for push model
                    XrDistChannel *dc = (XrDistChannel *) xr_calloc(1, sizeof(XrDistChannel));
                    if (dc) {
                        strncpy(dc->name, ch_name, XR_CHANNEL_NAME_MAX);
                        dc->is_owner = false;
                        dc->owner_node = node;
                        dc->cluster = c;
                        // Create local buffered channel for receiving PUSH data
                        dc->channel = xr_channel_new(c->isolate, remote_buf_size);
                        if (dc->channel) {
                            dc->channel->name = dc->name;
                            dc->channel->dist = dc;
                        }

                        uint32_t bucket = str_hash(ch_name) % XR_CLUSTER_CHANNEL_BUCKETS;

                        xr_amutex_lock(&c->channels_lock);
                        dc->next = c->channel_buckets[bucket];
                        c->channel_buckets[bucket] = dc;
                        c->channel_count++;
                        xr_amutex_unlock(&c->channels_lock);
                    }
                }
                break;
            }

            case XR_FRAME_CHANNEL_SUBSCRIBE: {
                XrFrameChannelSubscribe sub;
                if (xr_frame_decode_channel_subscribe(recv_buf, payload_len, &sub) == 0) {
                    xr_cluster_add_subscriber(c, sub.channel_name, node);
                }
                break;
            }

            case XR_FRAME_CHANNEL_UNSUBSCRIBE: {
                char unsub_name[XR_CHANNEL_NAME_MAX + 1];
                if (xr_frame_decode_channel_unsubscribe(recv_buf, payload_len, unsub_name,
                                                        sizeof(unsub_name)) == 0) {
                    xr_cluster_remove_subscriber(c, unsub_name, node);
                }
                break;
            }

            case XR_FRAME_CHANNEL_PUSH: {
                XrFrameChannelPush push;
                if (xr_frame_decode_channel_push(recv_buf, payload_len, &push) == 0) {
                    xr_cluster_channel_handle_push(c, push.channel_name, push.value_data,
                                                   push.value_len);
                }
                break;
            }

            case XR_FRAME_TOPIC_PUBLISH: {
                /*
                 * Wire format (see xr_cluster_topic_publish for rationale):
                 *
                 *   Legacy (pre-P17):
                 *     [topic_len 1B] [topic ...] [value_data ...]
                 *
                 *   Current (P17, hop-limited forwarding):
                 *     [topic_len 1B] [topic ...] [value_data ...] [hop 1B]
                 *
                 * Both are accepted here because the trailing hop byte was
                 * added without bumping the frame type or version. We
                 * detect the P17 form by requiring at least one byte
                 * beyond the advertised topic region AND accepting any
                 * value in [0, 255] as a legal hop count — a malformed
                 * payload that happens to have an extra byte would at
                 * worst cause an additional round of forwarding, bounded
                 * by the receiver's own hop decrement.
                 *
                 * A cleaner future version can bump to a dedicated
                 * TOPIC_PUBLISH_FWD frame type and deprecate the legacy
                 * form, but backward-compat during rolling upgrade is
                 * more valuable right now than wire-format purity.
                 */
                if (payload_len >= 2) {
                    uint8_t topic_len = recv_buf[0];
                    if (topic_len > 0 && 1 + topic_len <= payload_len) {
                        char topic[XR_TOPIC_PATTERN_MAX + 1];
                        if (topic_len <= XR_TOPIC_PATTERN_MAX) {
                            memcpy(topic, recv_buf + 1, topic_len);
                            topic[topic_len] = '\0';
                            uint32_t val_offset = 1 + topic_len;
                            uint32_t val_len = payload_len - val_offset;

                            // Detect presence of the trailing hop byte:
                            // the value region must be at least 1 byte
                            // AND we cannot tell a zero-length value with
                            // a single hop byte from an old zero-byte
                            // payload, so we treat val_len == 0 as legacy
                            // (no forwarding). Real publishes always have
                            // a non-empty serialized value.
                            uint8_t hop_limit = 0;
                            if (val_len >= 1) {
                                hop_limit = recv_buf[payload_len - 1];
                                val_len -= 1;  // exclude trailing hop byte
                            }

                            xr_cluster_topic_handle_publish(c, node, topic, recv_buf + val_offset,
                                                            val_len, hop_limit);
                        }
                    }
                }
                break;
            }

            case XR_FRAME_CORO_MONITOR: {
                char coro_name[XR_CORO_NAME_MAX + 1];
                if (xr_frame_decode_coro_monitor(recv_buf, payload_len, coro_name,
                                                 sizeof(coro_name)) == 0) {
                    xr_cluster_handle_coro_monitor(c, node, coro_name);
                }
                break;
            }

            case XR_FRAME_CORO_EXIT: {
                char coro_name[XR_CORO_NAME_MAX + 1];
                char reason[128];
                if (xr_frame_decode_coro_exit(recv_buf, payload_len, coro_name, sizeof(coro_name),
                                              reason, sizeof(reason)) == 0) {
                    xr_cluster_handle_coro_exit(c, coro_name, reason);
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

    // Node disconnected — cleanup subscribers before monitors
    xr_free(recv_buf);
    xr_cluster_remove_all_subscribers_for_node(c, node);
    xr_cluster_fire_monitors(c, node->name);
    if (c->on_node_removed)
        c->on_node_removed(node->name);
    xr_cluster_remove_node(c, node);
    xr_cluster_node_free(node);
}

/* ========== Event Callbacks ========== */

void xr_cluster_on_node_added(XrCluster *c, void (*cb)(const char *name)) {
    if (c)
        c->on_node_added = cb;
}

void xr_cluster_on_node_removed(XrCluster *c, void (*cb)(const char *name)) {
    if (c)
        c->on_node_removed = cb;
}

// xray binding: cluster.publish(topic, value)
static XrValue cluster_publish_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return xr_bool(false);

    XrString *topic_str = XR_TO_STRING(args[0]);
    int rc = xr_cluster_topic_publish(X, topic_str->data, args[1]);
    return xr_bool(rc == 0);
}

// xray binding: cluster.subscribe(pattern)
static XrValue cluster_subscribe_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *pattern_str = XR_TO_STRING(args[0]);
    XrChannel *ch = xr_cluster_topic_subscribe(X, pattern_str->data);
    if (!ch)
        return xr_null();
    return xr_value_from_channel(ch);
}

/* ========== Cluster Info API ========== */

static XrValue cluster_info_fn(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrJson *info = xr_json_new(NULL, 8);
    if (!info)
        return xr_null();

    // Self name
    XrString *self = xr_string_intern(X, c->self_name, (uint32_t) strlen(c->self_name), 0);
    xr_json_set_by_key(X, info, "self", xr_string_value(self));
    xr_json_set_by_key(X, info, "port", xr_int(c->listen_port));
    xr_json_set_by_key(X, info, "running", xr_bool(atomic_load(&c->running)));

    // Node list with metrics
    XrArray *node_arr = xr_array_new(NULL);
    if (node_arr) {
        xr_amutex_lock(&c->nodes_lock);
        XrClusterNode *node = c->nodes;
        while (node) {
            XrJson *nj = xr_json_new(NULL, 10);
            if (nj) {
                XrString *nname = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
                xr_json_set_by_key(X, nj, "name", xr_string_value(nname));

                XrString *nhost = xr_string_intern(X, node->host, (uint32_t) strlen(node->host), 0);
                xr_json_set_by_key(X, nj, "host", xr_string_value(nhost));
                xr_json_set_by_key(X, nj, "port", xr_int(node->port));
                xr_json_set_by_key(X, nj, "state", xr_int(node->state));

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
                xr_json_set_by_key(X, nj, "frames_sent",
                                   xr_int((int64_t) atomic_load(&node->metrics.frames_sent)));
                xr_json_set_by_key(X, nj, "frames_recv",
                                   xr_int((int64_t) atomic_load(&node->metrics.frames_recv)));
                xr_json_set_by_key(X, nj, "bytes_sent",
                                   xr_int((int64_t) atomic_load(&node->metrics.bytes_sent)));
                xr_json_set_by_key(X, nj, "bytes_recv",
                                   xr_int((int64_t) atomic_load(&node->metrics.bytes_recv)));
                // send_errors: writev short/fail counter — high values
                // flag a slow or lossy link; correlate with the slow
                // flag below.
                xr_json_set_by_key(X, nj, "send_errors",
                                   xr_int((int64_t) atomic_load(&node->metrics.send_errors)));
                // slow_consumer_events: total times this peer hit the
                // high watermark (4 MiB by default) since start. Each
                // event corresponds to one outq_bytes >= high_watermark
                // transition in cluster_node.
                xr_json_set_by_key(
                    X, nj, "slow_consumer_events",
                    xr_int((int64_t) atomic_load(&node->metrics.slow_consumer_events)));
                xr_json_set_by_key(X, nj, "rtt_ms", xr_int(node->metrics.last_rtt_ms));
                xr_json_set_by_key(X, nj, "outq_bytes", xr_int(node->outq.total_bytes));
                xr_json_set_by_key(X, nj, "outq_frames", xr_int(node->outq.frame_count));
                xr_json_set_by_key(X, nj, "slow", xr_bool(xr_cluster_node_is_slow(node)));

                // Phi accrual failure-detector score. Higher = more
                // likely dead. Threshold for "kill" is set by
                // cluster policy in cluster_health.c.
                int64_t now = xr_cluster_now_ms();
                double phi = xr_phi_value(&node->phi, now);
                xr_json_set_by_key(X, nj, "phi", xr_float(phi));
                xr_json_set_by_key(X, nj, "missed_heartbeats",
                                   xr_int((int64_t) node->missed_heartbeats));

                xr_array_push(node_arr, xr_json_value(nj));
            }
            node = node->next;
        }
        xr_amutex_unlock(&c->nodes_lock);
        xr_json_set_by_key(X, info, "nodes", xr_value_from_array(node_arr));
    }

    // Registries — best-effort counts. Each is guarded by its own
    // mutex under normal operation; a cross-registry snapshot is
    // intentionally lock-free here because info() is a diagnostic
    // endpoint and bounded staleness is preferable to global locking.
    xr_json_set_by_key(X, info, "channels", xr_int(c->channel_count));
    xr_json_set_by_key(X, info, "services", xr_int(c->service_count));
    xr_json_set_by_key(X, info, "topic_subs", xr_int(c->topic_sub_count));

    /*
     * Tombstone snapshot — number of nodes in the recently-dead
     * table. A non-zero value across successive calls means we have
     * peers that left the cluster within the past
     * XR_TOMBSTONE_WINDOW_MS (see cluster_health.c) and will be
     * refused if they try to rejoin. Useful for correlating "split
     * brain" scenarios.
     */
    xr_amutex_lock(&c->dead_nodes_lock);
    xr_json_set_by_key(X, info, "dead_nodes", xr_int(c->tombstone_count));
    xr_amutex_unlock(&c->dead_nodes_lock);

    /*
     * Expose the operator-configurable heartbeat knobs so ops can
     * sanity-check the live cluster against their YAML without
     * shelling into the node. These fields are rarely changed at
     * runtime but live at the XrCluster level so a snapshot is
     * trivially consistent.
     */
    xr_json_set_by_key(X, info, "heartbeat_interval_ms", xr_int(c->heartbeat_interval_ms));
    xr_json_set_by_key(X, info, "heartbeat_timeout_ms", xr_int(c->heartbeat_timeout_ms));
    xr_json_set_by_key(X, info, "max_missed_heartbeats", xr_int(c->max_missed_heartbeats));

    /*
     * TLS posture — one integer encoded as a small bitmap so a
     * single field tells the observer what security guarantees are
     * actually in force:
     *
     *   bit 0 (1): tls_enabled          — operator set tls.enabled
     *   bit 1 (2): has client context   — outbound uses TLS
     *   bit 2 (4): has server context   — inbound accepts TLS
     *
     * A mis-configured cluster (tls_enabled=true but no cert) shows
     * up as value 1: enabled but no contexts — useful because the
     * accept loop refuses all inbound in that state, and this field
     * lets the operator notice.
     */
    int tls_posture = 0;
    if (c->tls_enabled)
        tls_posture |= 1;
    if (c->tls_client_ctx)
        tls_posture |= 2;
    if (c->tls_server_ctx)
        tls_posture |= 4;
    xr_json_set_by_key(X, info, "tls", xr_int(tls_posture));

    return xr_json_value(info);
}

// Extended cluster.monitor: 1 arg = node monitor, 2 args = remote coro monitor
static XrValue cluster_monitor_coro_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    if (argc == 1) {
        // Node-level monitor: cluster.monitor("node_name")
        XrString *name_str = XR_TO_STRING(args[0]);
        XrChannel *ch = xr_cluster_monitor_node(X, name_str->data);
        if (!ch)
            return xr_null();
        return xr_value_from_channel(ch);
    }

    // Remote coroutine monitor: cluster.monitor("node_name", "coro_name")
    if (!XR_IS_STRING(args[1]))
        return xr_null();
    XrString *node_str = XR_TO_STRING(args[0]);
    XrString *coro_str = XR_TO_STRING(args[1]);

    XrChannel *ch = xr_cluster_monitor_coro(X, node_str->data, coro_str->data);
    if (!ch)
        return xr_null();
    return xr_value_from_channel(ch);
}

/* ========== Module Registration ========== */

XrModule *xr_load_module_cluster(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "cluster");

    XRS_EXPORT(mod, isolate, "start", cluster_start);
    XRS_EXPORT(mod, isolate, "join", cluster_join);
    XRS_EXPORT(mod, isolate, "self", cluster_self);
    XRS_EXPORT(mod, isolate, "nodes", cluster_nodes);
    XRS_EXPORT(mod, isolate, "channel", cluster_channel_fn);
    XRS_EXPORT(mod, isolate, "serve", cluster_serve_fn);
    XRS_EXPORT(mod, isolate, "reply", cluster_reply_fn);
    XRS_EXPORT(mod, isolate, "call", cluster_call_fn);
    XRS_EXPORT(mod, isolate, "monitor", cluster_monitor_coro_fn);
    XRS_EXPORT(mod, isolate, "discover", cluster_discover_fn);
    XRS_EXPORT(mod, isolate, "stop", cluster_stop_fn);
    XRS_EXPORT(mod, isolate, "info", cluster_info_fn);
    XRS_EXPORT(mod, isolate, "publish", cluster_publish_fn);
    XRS_EXPORT(mod, isolate, "subscribe", cluster_subscribe_fn);

    return mod;
}
