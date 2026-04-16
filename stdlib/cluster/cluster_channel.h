/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_channel.h - Named Channel distributed routing
 *
 * KEY CONCEPT:
 *   Implements the Owner model for Named Channels. The first node
 *   to create Channel(N, "name") becomes the Owner and holds the
 *   actual buffer. Other nodes get a Proxy that routes send/recv
 *   over TCP to the Owner.
 *
 * WHY THIS DESIGN:
 *   - Single Owner = single source of truth, FIFO guaranteed
 *   - Proxy send/recv are effectively RPC calls to Owner
 *   - No distributed queue coordination needed
 */

#ifndef XR_CLUSTER_CHANNEL_H
#define XR_CLUSTER_CHANNEL_H

#include "cluster.h"
#include "cluster_serial.h"
#include "../../src/coro/xchannel.h"
#include "../../src/runtime/value/xvalue.h"

/* ========== Dist Hook Implementation ========== */

/*
 * Install cluster dist hooks into the global xchannel hook pointer.
 * Called once during cluster startup.
 */
void xr_cluster_channel_install_hooks(void);

/*
 * Uninstall cluster dist hooks (restore NULL).
 * Called during cluster shutdown.
 */
void xr_cluster_channel_uninstall_hooks(void);

/* ========== Named Channel Operations ========== */

/*
 * Handle an incoming CHANNEL_SEND frame from a remote node.
 * Writes the value into the local Owner channel's buffer.
 */
int xr_cluster_channel_handle_send(XrCluster *c, const char *channel_name,
                                    const uint8_t *value_data, uint32_t value_len);

/*
 * Handle an incoming CHANNEL_CLOSE frame from a remote node.
 */
void xr_cluster_channel_handle_close(XrCluster *c, const char *channel_name);

/*
 * Synchronize Named Channel info with a newly connected node.
 * Sends CHANNEL_SYNC frames for all locally owned channels.
 */
void xr_cluster_channel_sync_to_node(XrCluster *c, XrClusterNode *node);

/* ========== Push Model (for Proxy Channel select support) ========== */

/*
 * Push data from Owner channel buffer to one subscriber (round-robin).
 * Called after data is written to an Owner channel that has subscribers.
 */
void xr_cluster_channel_push_to_subscribers(XrCluster *c, const char *name);

/*
 * Handle incoming CHANNEL_PUSH frame on Proxy side.
 * Writes value into Proxy channel's local buffer and wakes select waiters.
 */
int xr_cluster_channel_handle_push(XrCluster *c, const char *channel_name,
                                    const uint8_t *value_data, uint32_t value_len);

/*
 * Send SUBSCRIBE frame to Owner node for a Proxy channel.
 * Called when Proxy channel enters select.
 */
void xr_cluster_channel_subscribe(XrChannel *ch);

/*
 * Send UNSUBSCRIBE frame to Owner node for a Proxy channel.
 * Called when Proxy channel exits select.
 */
void xr_cluster_channel_unsubscribe(XrChannel *ch);

#endif
