/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster.h - Distributed cluster module public API
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

#ifndef XR_CLUSTER_H
#define XR_CLUSTER_H

#include "cluster_node.h"
#include "cluster_proto.h"
#include "cluster_discovery.h"
#include "../../src/coro/xchannel.h"
#include "../../src/module/xmodule.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* ========== Forward Declarations ========== */

struct XrayIsolate;
struct XrChannel;
typedef struct XrCluster XrCluster;

/* ========== Channel Subscriber (for select push model) ========== */

#define XR_MAX_SUBSCRIBERS 32

typedef struct XrChannelSubscribers {
    XrClusterNode *nodes[XR_MAX_SUBSCRIBERS];  // subscriber node array
    int count;
} XrChannelSubscribers;

/* ========== Distributed Channel Entry ========== */

typedef struct XrDistChannel {
    char name[XR_CHANNEL_NAME_MAX + 1];
    bool is_owner;                  // true = this node owns the buffer
    XrClusterNode *owner_node;      // valid when is_owner == false
    struct XrChannel *channel;      // local XrChannel object
    struct XrDistChannel *next;     // hash chain
    XrCluster *cluster;             // owning cluster instance
    XrChannelSubscribers subscribers;       // subscriber array (Owner only)
    int rr_index;                           // round-robin index for push
} XrDistChannel;

/* ========== Service Entry ========== */

typedef struct XrServiceEntry {
    char name[XR_SERVICE_NAME_MAX + 1];
    struct XrChannel *request_ch;   // Channel to deliver incoming requests
    struct XrServiceEntry *next;
} XrServiceEntry;

/* ========== Forward Declarations ========== */

typedef struct XrRemoteCoroMonitor XrRemoteCoroMonitor;

/* ========== Cluster State ========== */

#define XR_CLUSTER_CHANNEL_BUCKETS 64
#define XR_CLUSTER_SERVICE_BUCKETS 16
#define XR_CLUSTER_TOPIC_BUCKETS   32
#define XR_TOPIC_PATTERN_MAX       127

typedef struct XrCluster {
    char              self_name[XR_NODE_NAME_MAX + 1];
    uint16_t          listen_port;
    char              secret[64];
    int               listen_fd;
    struct XrayIsolate *isolate;

    // Connected nodes (linked list, protected by nodes_lock)
    XrClusterNode    *nodes;
    int               node_count;
    XrSpinlock        nodes_lock;

    // Named Channel registry (hash table, protected by channels_lock)
    XrDistChannel    *channel_buckets[XR_CLUSTER_CHANNEL_BUCKETS];
    int               channel_count;
    XrSpinlock        channels_lock;

    // Service registry (hash table)
    XrServiceEntry   *service_buckets[XR_CLUSTER_SERVICE_BUCKETS];
    int               service_count;
    XrSpinlock        services_lock;

    // Topic Pub/Sub registry (hash table of subscriptions)
    struct XrTopicSubscription *topic_buckets[XR_CLUSTER_TOPIC_BUCKETS];
    int               topic_sub_count;
    XrSpinlock        topics_lock;

    // Request ID counter for service calls
    _Atomic(uint64_t) next_request_id;

    // Heartbeat configuration
    int               heartbeat_interval_ms;  // default 5000
    int               heartbeat_timeout_ms;   // default 15000 (3x interval)
    int               max_missed_heartbeats;  // default 3

    // Dead nodes tombstone (prevent reconnecting to recently departed nodes)
    struct {
        char     name[XR_NODE_NAME_MAX + 1];
        int64_t  time;
    }                *tombstones;        // dynamic array
    int               tombstone_count;
    int               tombstone_cap;
    XrSpinlock        dead_nodes_lock;

    // Node event callbacks
    void (*on_node_added)(const char *name);
    void (*on_node_removed)(const char *name);

    // Node monitors (CSP-style: Channel receives notification on disconnect)
    struct XrNodeMonitor *monitors;
    int               monitor_count;
    XrSpinlock        monitors_lock;

    // Running state
    _Atomic(bool)     running;

    // Heartbeat thread (drives send_heartbeats + check_heartbeats)
    pthread_t         heartbeat_thread;
    bool              heartbeat_thread_started;

    // Remote coroutine monitors (linked list)
    XrRemoteCoroMonitor *remote_coro_monitors;

    // LAN auto-discovery (NULL if not enabled)
    XrClusterDiscovery *discovery;
} XrCluster;

/* ========== Cluster Lifecycle API ========== */

// Initialize and start the cluster node
// config contains: name, port, secret
int xr_cluster_start(struct XrayIsolate *X, const char *name,
                      uint16_t port, const char *secret);

// Connect to a remote node (host:port)
int xr_cluster_join(XrCluster *c, const char *host, uint16_t port);

// Stop the cluster and close all connections
void xr_cluster_stop(XrCluster *c);

// Check if cluster is running
bool xr_cluster_is_running(XrCluster *c);

// Get self node name
const char *xr_cluster_self_name(XrCluster *c);

/* ========== Node Query API ========== */

// Find a node by name (must hold nodes_lock or accept stale reads)
XrClusterNode *xr_cluster_find_node(XrCluster *c, const char *name);

// Add a connected node to the cluster
void xr_cluster_add_node(XrCluster *c, XrClusterNode *node);

// Remove a node from the cluster
void xr_cluster_remove_node(XrCluster *c, XrClusterNode *node);

/* ========== Named Channel Registry ========== */

// Register a Named Channel (called when Channel(N, "name") is created)
void xr_cluster_register_channel(XrCluster *c, const char *name, struct XrChannel *ch);

// Lookup a Named Channel by name
XrDistChannel *xr_cluster_find_channel(XrCluster *c, const char *name);

// Unregister a Named Channel
void xr_cluster_unregister_channel(XrCluster *c, const char *name);

/* ========== Service Registry ========== */

// Register a service (returns request channel)
struct XrChannel *xr_cluster_register_service(struct XrayIsolate *X,
                                               const char *name);

// Find a service by name
XrServiceEntry *xr_cluster_find_service(XrCluster *c, const char *name);

/* ========== Frame Processing ========== */

// Process incoming frames from a connected node (runs in a coroutine)
void xr_cluster_process_node(XrCluster *c, XrClusterNode *node);

/* ========== Health & Robustness ========== */

// Check all nodes for heartbeat timeout, disconnect dead ones
void xr_cluster_check_heartbeats(XrCluster *c);

// Send heartbeat pings to all connected nodes
void xr_cluster_send_heartbeats(XrCluster *c);

// Reconnect to a node with exponential backoff
// base_ms: initial delay (default 500), max_ms: cap (default 30000)
int xr_cluster_reconnect(XrCluster *c, const char *host, uint16_t port,
                          int base_ms, int max_ms, int max_attempts);

// Gossip: send NODE_INFO to a peer (list of known nodes)
void xr_cluster_gossip_to_node(XrCluster *c, XrClusterNode *node);

// Handle incoming NODE_INFO gossip frame
void xr_cluster_handle_node_info(XrCluster *c, const uint8_t *payload, uint32_t len);

// Dead node tombstone management
void xr_cluster_mark_dead(XrCluster *c, const char *name);
bool xr_cluster_is_dead(XrCluster *c, const char *name);
void xr_cluster_sweep_tombstones(XrCluster *c, int64_t max_age_ms);

// Set node event callbacks
void xr_cluster_on_node_added(XrCluster *c, void (*cb)(const char *name));
void xr_cluster_on_node_removed(XrCluster *c, void (*cb)(const char *name));

/* ========== Node Monitor (CSP-style fault detection) ========== */

typedef struct XrNodeMonitor {
    char node_name[XR_NODE_NAME_MAX + 1];  // "*" = monitor all nodes
    struct XrChannel *notify_ch;            // Receives node name string on disconnect
    struct XrNodeMonitor *next;
} XrNodeMonitor;

// Monitor a specific node. Returns a Channel that receives the node name
// as a string when that node disconnects. Use "*" to monitor all nodes.
struct XrChannel *xr_cluster_monitor_node(struct XrayIsolate *X, const char *node_name);

// Fire monitors for a disconnected node (called internally)
void xr_cluster_fire_monitors(XrCluster *c, const char *node_name);

/* ========== Topic Pub/Sub ========== */

typedef struct XrTopicSubscription {
    char pattern[XR_TOPIC_PATTERN_MAX + 1]; // e.g. "events.*" or "chat.room1"
    struct XrChannel *notify_ch;            // Delivers published values
    struct XrTopicSubscription *next;       // Hash chain
} XrTopicSubscription;

// Subscribe to a topic pattern. Returns a Channel that receives published values.
// Supports wildcard: "*" matches one segment, ">" matches remaining segments.
// Example: "events.*" matches "events.user" but not "events.user.login"
//          "events.>" matches "events.user" and "events.user.login"
struct XrChannel *xr_cluster_topic_subscribe(struct XrayIsolate *X,
                                              const char *pattern);

// Unsubscribe from a topic pattern
void xr_cluster_topic_unsubscribe(XrCluster *c, const char *pattern);

// Publish a value to a topic. Delivers to all matching local subscriptions
// and forwards to all connected nodes.
int xr_cluster_topic_publish(struct XrayIsolate *X,
                              const char *topic, XrValue value);

// Handle incoming TOPIC_PUBLISH frame from a remote node
void xr_cluster_topic_handle_publish(XrCluster *c, const char *topic,
                                      const uint8_t *value_data, uint32_t value_len);

// Deliver to local subscribers matching the topic
void xr_cluster_topic_deliver_local(XrCluster *c, const char *topic, XrValue value);

// Check if a pattern matches a topic string
bool xr_topic_match(const char *pattern, const char *topic);

/* ========== Remote Coroutine Monitoring ========== */

// Remote monitor entry: tracks which remote coroutine we're monitoring
typedef struct XrRemoteCoroMonitor {
    char node_name[64];                      // Remote node name
    char coro_name[128];                     // Remote coroutine name
    struct XrChannel *notify_ch;             // Local channel for exit notification
    struct XrRemoteCoroMonitor *next;
} XrRemoteCoroMonitor;

// Monitor a coroutine on a remote node. Returns a Channel that receives
// exit reason string when the remote coroutine terminates.
// cluster.monitor("node_name", "coro_name")
struct XrChannel *xr_cluster_monitor_coro(struct XrayIsolate *X,
                                           const char *node_name,
                                           const char *coro_name);

// Handle incoming CORO_EXIT frame from a remote node
void xr_cluster_handle_coro_exit(XrCluster *c, const char *coro_name, const char *reason);

// Handle incoming CORO_MONITOR request from a remote node
void xr_cluster_handle_coro_monitor(XrCluster *c, struct XrClusterNode *node,
                                     const char *coro_name);

/* ========== Subscriber Management (for select push model) ========== */

void xr_cluster_add_subscriber(XrCluster *c, const char *channel_name, XrClusterNode *node);
void xr_cluster_remove_subscriber(XrCluster *c, const char *channel_name, XrClusterNode *node);
void xr_cluster_remove_all_subscribers_for_node(XrCluster *c, XrClusterNode *node);

/* ========== Module Registration ========== */

XrModule *xr_load_module_cluster(struct XrayIsolate *isolate);

/* ========== Cluster Info API ========== */

// Returns Json with full cluster state, node metrics, phi values
// Exposed as cluster.info() in xray

#endif
