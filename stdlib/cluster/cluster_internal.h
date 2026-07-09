/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_internal.h - Private distributed cluster runtime/data-plane API
 *
 * KEY CONCEPT:
 *   Decentralized cluster with Named Channel as the core abstraction.
 *   Each node is identified by a unique name. Nodes connect via TCP
 *   with challenge-response handshake (SHA-256).
 *
 * WHY THIS DESIGN:
 *   - Owner model for Named Channels: single source of truth, no
 *     distributed queue coordination needed
 *   - Pure C module, no .xr layer: all functions are I/O or yieldable
 *   - at-most-once delivery: consistent with Go/Erlang/NATS semantics
 */

#ifndef XR_CLUSTER_INTERNAL_H
#define XR_CLUSTER_INTERNAL_H

#include "cluster.h"
#include "cluster_node.h"
#include "cluster_proto.h"
#include "../../src/base/xdefs.h"
#include "../../src/coro/xchannel.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/value/xvalue.h"
#include "../net/tls.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../../src/os/os_thread.h"

/* ========== Forward Declarations ========== */

struct XrVMRuntime;
struct XrChannel;
typedef struct XrCluster XrCluster;
typedef struct XrClusterDiscovery XrClusterDiscovery;

/* ========== Channel Subscriber (for select push model) ========== */

#define XR_MAX_SUBSCRIBERS 32

typedef struct XrChannelSubscribers {
    XrClusterNode *nodes[XR_MAX_SUBSCRIBERS];  // subscriber node array
    int count;
} XrChannelSubscribers;

/* ========== Distributed Channel Entry ========== */

typedef struct XrDistChannel {
    char name[XR_CHANNEL_NAME_MAX + 1];
    bool is_owner;                     // true = this node owns the buffer
    XrClusterNode *owner_node;         // valid when is_owner == false
    struct XrChannel *channel;         // local XrChannel object
    struct XrDistChannel *next;        // hash chain
    XrCluster *cluster;                // owning cluster instance
    XrChannelSubscribers subscribers;  // subscriber array (Owner only)
    int rr_index;                      // round-robin index for push
} XrDistChannel;

/* ========== Service Entry ========== */

typedef struct XrServiceEntry {
    char name[XR_SERVICE_NAME_MAX + 1];
    struct XrChannel *request_ch;  // Channel to deliver incoming requests
    struct XrServiceEntry *next;
} XrServiceEntry;

/* ========== Value Wire Serialization ========== */

typedef struct XrSerialBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool error;
} XrSerialBuf;

typedef struct XrSerialReader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int depth;
    struct XrVMRuntime *X;
} XrSerialReader;

void cluster_serial_buf_init(XrSerialBuf *buf);
void cluster_serial_buf_free(XrSerialBuf *buf);
int cluster_encode(struct XrVMRuntime *X, XrValue value, XrSerialBuf *buf);
void cluster_serial_reader_init(XrSerialReader *r, struct XrVMRuntime *X, const uint8_t *data,
                                size_t len);
int cluster_decode(XrSerialReader *r, XrValue *out);

static inline int cluster_decode_value(struct XrVMRuntime *X, const uint8_t *data, size_t len,
                                       XrValue *out) {
    XrSerialReader r;
    cluster_serial_reader_init(&r, X, data, len);
    return cluster_decode(&r, out);
}

/* ========== Forward Declarations ========== */

typedef struct XrRemoteCoroMonitor XrRemoteCoroMonitor;

/* ========== Cluster State ========== */

#define XR_CLUSTER_CHANNEL_BUCKETS 64
#define XR_CLUSTER_SERVICE_BUCKETS 16
#define XR_TOPIC_PATTERN_MAX 127

struct XrTopicTrieNode;  // forward decl — definition in cluster_topic.c

typedef struct XrCluster {
    char self_name[XR_NODE_NAME_MAX + 1];
    uint16_t listen_port;
    char secret[64];
    int listen_fd;
    struct XrVMRuntime *isolate;

    // Connected nodes (linked list, protected by nodes_lock)
    XrClusterNode *nodes;
    int node_count;
    XrAdaptiveMutex nodes_lock;

    // Named Channel registry (hash table, protected by channels_lock)
    XrDistChannel *channel_buckets[XR_CLUSTER_CHANNEL_BUCKETS];
    int channel_count;
    XrAdaptiveMutex channels_lock;

    // Service registry (hash table)
    XrServiceEntry *service_buckets[XR_CLUSTER_SERVICE_BUCKETS];
    int service_count;
    XrAdaptiveMutex services_lock;

    /*
     * Topic Pub/Sub registry.
     *
     * Route lookups go through a NATS-style segment trie (see
     * cluster_topic.c) instead of the old flat hash of subscriptions.
     * The trie makes publish() cost O(topic_depth) instead of
     * O(total_subscriptions) — critical for services that maintain
     * thousands of subscriptions of which only a handful match any
     * given message. The root node is embedded to keep the hot path
     * a single dereference.
     *
     * topic_root is always live between start_ex and stop; stop
     * recursively destroys the tree and resets it back to an empty
     * root.
     */
    struct XrTopicTrieNode *topic_root;  // trie root; NULL before init
    int topic_sub_count;
    XrAdaptiveMutex topics_lock;

    // Request ID counter for service calls
    _Atomic(uint64_t) next_request_id;

    // Heartbeat configuration
    int heartbeat_interval_ms;  // default 5000
    int heartbeat_timeout_ms;   // default 15000 (3x interval)
    int max_missed_heartbeats;  // default 3

    // Per-node pending request cap (default XR_MAX_PENDING_REQUESTS).
    // Controls backpressure on concurrent RPC / channel recv proxies.
    int max_pending_requests;

    // Dead node tombstones prevent immediate rejoin of recently departed nodes.
    struct {
        char name[XR_NODE_NAME_MAX + 1];
        int64_t time;
    } *tombstones;  // dynamic array
    int tombstone_count;
    int tombstone_cap;
    XrAdaptiveMutex dead_nodes_lock;

    // Node monitors (CSP-style: Channel receives notification on disconnect)
    struct XrNodeMonitor *monitors;
    int monitor_count;
    XrAdaptiveMutex monitors_lock;

    // Running state
    _Atomic(bool) running;

    /*
     * Stop-signalling pipe for coroutine-friendly interruptible sleep.
     *
     * Every long-lived cluster coroutine uses
     * cluster_sleep_interruptible(c, ms) which reads from
     * stop_pipe[0] with a read deadline. cluster_runtime_stop closes
     * stop_pipe[1] early, turning every outstanding read into an
     * immediate EOF. Both ends non-blocking.
     *
     * Created in start_ex — pipe() failure is fatal.
     */
    int stop_pipe[2];

    /*
     * Heartbeat coroutine — spawned in cluster_runtime_start, yields via
     * cluster_sleep_interruptible between ticks, observes running
     * (+ EOF on stop_pipe) to exit.
     *
     * heartbeat_running is flipped to false by the coroutine on exit so
     * cluster_runtime_stop can wait briefly before freeing the cluster
     * state the coroutine still references. Lives in the same style as
     * accept_coro_spawned / accept_running below.
     */
    bool heartbeat_coro_spawned;
    _Atomic(bool) heartbeat_running;

    /*
     * Inbound-accept coroutine state.
     *
     *   accept_coro_spawned — true once cluster_runtime_start successfully
     *                         spawned the accept coroutine. Prevents
     *                         double-spawn and lets cluster_runtime_stop know
     *                         whether to wait for it at teardown.
     *   accept_running      — flipped to false by the coro on exit so
     *                         cluster_runtime_stop can spin-wait briefly and
     *                         avoid tearing down node state while the
     *                         accept path is still inside
     *                         cluster_node_accept.
     */
    bool accept_coro_spawned;
    _Atomic(bool) accept_running;

    // Remote coroutine monitors (linked list)
    XrRemoteCoroMonitor *remote_coro_monitors;

    // LAN auto-discovery (NULL if not enabled)
    XrClusterDiscovery *discovery;

    /*
     * Optional inter-node TLS wrap (see cluster_runtime_start).
     *
     *   tls_enabled     — flip to turn on TLS for every inbound and
     *                     outbound cluster connection.
     *   tls_client_ctx  — used by cluster_node_connect when TLS is on.
     *                     Built at start_ex time with caller-supplied CA
     *                     bundle, optional client cert/key (for mTLS), and
     *                     optional verify_peer toggle.
     *   tls_server_ctx  — used by cluster_accept_loop to wrap inbound fds
     *                     via xr_io_accept_tls_with_ctx. NULL if the
     *                     operator did not supply a cert+key pair; in
     *                     that case tls_enabled + NULL tls_server_ctx
     *                     causes the accept loop to refuse all inbound
     *                     connections rather than silently downgrade.
     *
     * These fields stay NULL/false when TLS is not requested.
     */
    bool tls_enabled;
    XrTlsContext *tls_client_ctx;
    XrTlsContext *tls_server_ctx;
} XrCluster;

/* ========== Cluster Lifecycle API ========== */

/*
 * TLS options for cluster_runtime_start.
 *
 *   enabled          — master switch. When false the other fields are
 *                      ignored and the cluster reverts to plain TCP.
 *
 *   ca_file          — path to a PEM bundle used to verify peer
 *                      certificates. Pass NULL to fall back on the
 *                      system trust store (TLS contexts are created
 *                      with SSL_CTX_set_default_verify_paths by
 *                      default). If the path ends in '/' it is
 *                      interpreted as a directory (OpenSSL CApath).
 *
 *   cert_file,
 *   key_file         — optional server certificate and private key in
 *                      PEM format. Supplying both enables mTLS /
 *                      inbound TLS accept. Leaving them NULL builds a
 *                      client-only cluster (still useful: outgoing
 *                      join traffic is encrypted).
 *
 *   insecure         — disable peer certificate verification. Set true
 *                      only for development / self-signed sandboxes.
 *                      This is a loaded footgun in production and
 *                      should be logged by callers.
 *
 * All string pointers are borrowed for the duration of the call; the
 * contents are copied into OpenSSL contexts that live until
 * cluster_runtime_stop.
 */
typedef struct XrClusterTlsOptions {
    bool enabled;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    bool insecure;
} XrClusterTlsOptions;

/*
 * Start a cluster with explicit TLS options. Passing `tls == NULL` starts
 * a plain TCP cluster.
 */
int cluster_runtime_start(struct XrVMRuntime *X, const char *name, uint16_t port,
                          const char *secret, const XrClusterTlsOptions *tls);

// Connect to a remote node (host:port)
int cluster_runtime_join(XrCluster *c, const char *host, uint16_t port);

// Stop the cluster and close all connections
void cluster_runtime_stop(XrCluster *c);

// Check if cluster is running
bool cluster_runtime_is_running(XrCluster *c);

/*
 * Coroutine-friendly interruptible sleep.
 *
 * Sleeps for up to `ms` milliseconds or until cluster_runtime_stop signals
 * shutdown. Intended for use inside cluster-owned native coroutines
 * (heartbeat, accept retry, discovery tick) that
 * need periodic wake-up without blocking the worker thread with
 * nanosleep/usleep.
 *
 * Mechanism: reads from the cluster's stop_pipe[0] with a read
 * deadline set via xr_socket_set_read_timeout. cluster_runtime_stop closes
 * stop_pipe[1] early so every outstanding read returns EOF
 * immediately. Each worker's netpoll integration unblocks the
 * coroutine on deadline even if no data arrives.
 *
 * Returns:
 *   true  — the full `ms` elapsed normally (continue looping)
 *   false — the cluster was stopped or stop_pipe is unavailable
 *           (caller should exit its loop)
 *
 * Falls back to a plain nanosleep if stop_pipe was never set up
 * (pipe() failed at start_ex); the cluster is still functional in
 * that case, just not interruptible on the sleep boundary.
 */
bool cluster_sleep_interruptible(XrCluster *c, int ms);

// Get self node name
const char *cluster_runtime_self_name(XrCluster *c);

/* ========== Node Query API ========== */

// Find a node by name (must hold nodes_lock or accept stale reads)
XrClusterNode *cluster_node_find(XrCluster *c, const char *name);

// Add a connected node to the cluster
void cluster_node_add(XrCluster *c, XrClusterNode *node);

// Remove a node from the cluster
void cluster_node_remove(XrCluster *c, XrClusterNode *node);

/* ========== Named Channel Registry ========== */

// Register a Named Channel (called when Channel(N, "name") is created)
void cluster_channel_register(XrCluster *c, const char *name, struct XrChannel *ch);

// Lookup a Named Channel by name
XrDistChannel *cluster_channel_find(XrCluster *c, const char *name);

// Unregister a Named Channel
void cluster_channel_unregister(XrCluster *c, const char *name);

/* ========== Named Channel Distributed Routing ========== */

void cluster_channel_install_hooks(struct XrVMRuntime *X);
void cluster_channel_uninstall_hooks(struct XrVMRuntime *X);

int cluster_channel_handle_send(XrCluster *c, const char *channel_name,
                                const uint8_t *value_data, uint32_t value_len);
void cluster_channel_handle_close(XrCluster *c, const char *channel_name);
void cluster_channel_push_to_subscribers(XrCluster *c, const char *name);
int cluster_channel_handle_push(XrCluster *c, const char *channel_name,
                                const uint8_t *value_data, uint32_t value_len);
void cluster_channel_subscribe(struct XrChannel *ch);
void cluster_channel_unsubscribe(struct XrChannel *ch);

/* ========== Service Registry ========== */

// Register a service (returns request channel)
struct XrChannel *cluster_service_register(struct XrVMRuntime *X, const char *name);

// Find a service by name
XrServiceEntry *cluster_service_find(XrCluster *c, const char *name);

/* ========== Frame Processing ========== */

// Process incoming frames from a connected node (runs in a coroutine)
void cluster_process_node(XrCluster *c, XrClusterNode *node);

/* ========== Health & Robustness ========== */

// Check all nodes for heartbeat timeout, disconnect dead ones
void cluster_health_check_heartbeats(XrCluster *c);

// Send heartbeat pings to all connected nodes
void cluster_health_send_heartbeats(XrCluster *c);

// Dead node tombstone management
void cluster_health_mark_dead(XrCluster *c, const char *name);
bool cluster_health_is_dead(XrCluster *c, const char *name);

/* ========== Node Monitor (CSP-style fault detection) ========== */

typedef struct XrNodeMonitor {
    char node_name[XR_NODE_NAME_MAX + 1];  // "*" = monitor all nodes
    struct XrChannel *notify_ch;           // Receives node name string on disconnect
    struct XrNodeMonitor *next;
} XrNodeMonitor;

// Monitor a specific node. Returns a Channel that receives the node name
// as a string when that node disconnects. Use "*" to monitor all nodes.
struct XrChannel *cluster_monitor_node(struct XrVMRuntime *X, const char *node_name);

// Fire monitors for a disconnected node (called internally)
void cluster_monitor_fire(XrCluster *c, const char *node_name);

/* ========== Topic Pub/Sub ========== */

typedef struct XrTopicSubscription {
    char pattern[XR_TOPIC_PATTERN_MAX + 1];  // e.g. "events.*" or "chat.room1"
    struct XrChannel *notify_ch;             // Delivers published values
    struct XrTopicSubscription *next;        // Hash chain
} XrTopicSubscription;

// Subscribe to a topic pattern. Returns a Channel that receives published values.
// Supports wildcard: "*" matches one segment, ">" matches remaining segments.
// Example: "events.*" matches "events.user" but not "events.user.login"
//          "events.>" matches "events.user" and "events.user.login"
struct XrChannel *cluster_topic_subscribe(struct XrVMRuntime *X, const char *pattern);

// Publish a value to a topic. Delivers to all matching local subscriptions
// and forwards to all connected nodes.
int cluster_topic_publish(struct XrVMRuntime *X, const char *topic, XrValue value);

/*
 * Handle incoming TOPIC_PUBLISH frame from a remote node.
 *
 *   from       — the XrClusterNode the frame arrived from, used for
 *                split-horizon so we never echo the frame back to
 *                its sender. NULL is legal (locally-injected test
 *                frames etc.) but in production always non-NULL.
 *   hop_limit  — the hop count encoded in the frame's fixed header.
 *                0 means "deliver locally only, do not re-forward"
 *                Non-zero causes a decrement and a re-send to every
 *                other connected peer.
 *
 * The value is delivered to local subscribers regardless of hop_limit.
 */
void cluster_topic_handle_publish(XrCluster *c, struct XrClusterNode *from, const char *topic,
                                  const uint8_t *value_data, uint32_t value_len,
                                  uint8_t hop_limit);

// Deliver to local subscribers matching the topic
void cluster_topic_deliver_local(XrCluster *c, const char *topic, XrValue value);

/*
 * Topic trie lifecycle. cluster_topics_init must be called once
 * after topics_lock is initialised and before any subscribe path is
 * exposed. cluster_topics_destroy closes every subscriber channel,
 * recursively frees the trie, and resets topic_root to NULL — call it
 * exactly once from cluster_runtime_stop (the function tolerates a NULL
 * root, so double-stop is safe).
 */
int cluster_topics_init(XrCluster *c);
void cluster_topics_destroy(XrCluster *c);

/* ========== Remote Coroutine Monitoring ========== */

// Remote monitor entry: tracks which remote coroutine we're monitoring
typedef struct XrRemoteCoroMonitor {
    char node_name[64];           // Remote node name
    char coro_name[128];          // Remote coroutine name
    struct XrChannel *notify_ch;  // Local channel for exit notification
    struct XrRemoteCoroMonitor *next;
} XrRemoteCoroMonitor;

// Monitor a coroutine on a remote node. Returns a Channel that receives
// exit reason string when the remote coroutine terminates.
// cluster.monitor("node_name", "coro_name")
struct XrChannel *cluster_monitor_coro(struct XrVMRuntime *X, const char *node_name,
                                       const char *coro_name);

// Handle incoming CORO_EXIT frame from a remote node
void cluster_monitor_handle_coro_exit(XrCluster *c, const char *coro_name, const char *reason);

// Handle incoming CORO_MONITOR request from a remote node
void cluster_monitor_handle_coro_request(XrCluster *c, struct XrClusterNode *node,
                                         const char *coro_name);

/* ========== Subscriber Management (for select push model) ========== */

void cluster_subscriber_add(XrCluster *c, const char *channel_name, XrClusterNode *node);
void cluster_subscriber_remove(XrCluster *c, const char *channel_name, XrClusterNode *node);
void cluster_subscriber_remove_all_for_node(XrCluster *c, XrClusterNode *node);

/* ========== LAN Discovery ========== */

int cluster_discovery_start(XrCluster *c);
void cluster_discovery_stop(XrCluster *c);

/* ========== Cluster Info API ========== */

// Returns Json with full cluster state, node metrics, phi values
// Exposed as cluster.info() in xray

#endif
