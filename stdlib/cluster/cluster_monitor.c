/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_monitor.c - Node and remote coroutine monitoring
 *
 * KEY CONCEPT:
 *   CSP-style monitoring: watchers receive notifications via Channels
 *   when a node disconnects or a remote coroutine exits.
 */

#include "cluster.h"
#include "cluster_node.h"
#include "cluster_proto.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoro_registry.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/vm/xvm_internal.h"

#include <stdlib.h>
#include <string.h>

// xr_coro_create_native is not declared in any public header
extern struct XrCoroutine *xr_coro_create_native(struct XrayIsolate *X, void (*func)(void *),
                                                 void *arg, const char *name);

/* ========== Node Monitor (CSP-style) ========== */

/*
 * Register a node-down monitor and return the notification channel.
 *
 * Delivery policy — AT-MOST-ONCE (buffered(8), drop on overflow):
 *
 *   - The returned channel has a fixed capacity of 8. Each
 *     xr_cluster_fire_monitors invocation pushes ONE message (the
 *     dead node's name) via try_send. If the channel is full at
 *     that instant, the notification is DROPPED silently.
 *
 *   - This matters under "flap" scenarios: if the same peer goes
 *     down, reconnects, and dies again 9+ times before the script
 *     drained the channel, the 10th death event is lost. In
 *     practice applications drain the channel in a tight loop from
 *     a dedicated supervisor coroutine, so the window for overflow
 *     is narrow and losing "yet another" identical notification is
 *     acceptable.
 *
 *   - Applications that cannot tolerate drops should EITHER
 *     (a) drain the channel synchronously inside their supervisor
 *     loop so the buffer never fills, OR
 *     (b) combine the monitor with cluster_info() so the current
 *     dead set can be queried on each notification and any missed
 *     deaths are recovered.
 *
 * Capacity 8 was chosen to be large enough that a supervisor
 * running at normal scheduling cadence never loses notifications,
 * while small enough that a wedged supervisor cannot accumulate
 * unbounded memory. Callers that need a larger buffer can subscribe
 * to cluster_info() metrics instead.
 */
XrChannel *xr_cluster_monitor_node(XrayIsolate *X, const char *node_name) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !node_name)
        return NULL;

    XrNodeMonitor *m = (XrNodeMonitor *) xr_calloc(1, sizeof(XrNodeMonitor));
    if (!m)
        return NULL;

    strncpy(m->node_name, node_name, XR_NODE_NAME_MAX);
    m->node_name[XR_NODE_NAME_MAX] = '\0';

    // Buffered(8) channel for notifications — see comment above for
    // the drop-on-overflow policy rationale.
    XrChannel *ch = xr_channel_new(X, 8);
    if (!ch) {
        xr_free(m);
        return NULL;
    }
    m->notify_ch = ch;

    xr_mutex_lock(&c->monitors_lock);
    m->next = c->monitors;
    c->monitors = m;
    c->monitor_count++;
    xr_mutex_unlock(&c->monitors_lock);

    return ch;
}

void xr_cluster_fire_monitors(XrCluster *c, const char *node_name) {
    if (!c || !node_name)
        return;

/*
 * Collect matching notify_ch pointers under the lock, deliver outside.
 * Rationale: xr_channel_try_send wakes waiters that may run user
 * callbacks (e.g. a supervisor that calls cluster.monitor on another
 * node), which would recursively grab monitors_lock.
 *
 * 64 notifications per disconnect is way more than any realistic
 * monitor set; overflow is dropped, matching the at-most-once spirit.
 */
#define XR_MON_FIRE_MAX 64
    struct XrChannel *targets[XR_MON_FIRE_MAX];
    int target_count = 0;

    xr_mutex_lock(&c->monitors_lock);
    XrNodeMonitor *m = c->monitors;
    while (m && target_count < XR_MON_FIRE_MAX) {
        // Match specific name or wildcard "*"
        if (strcmp(m->node_name, "*") == 0 || strcmp(m->node_name, node_name) == 0) {
            if (m->notify_ch && !xr_channel_is_closed(m->notify_ch)) {
                targets[target_count++] = m->notify_ch;
            }
        }
        m = m->next;
    }
    xr_mutex_unlock(&c->monitors_lock);

    // Intern the node_name once, reuse across deliveries
    XrString *str = xr_string_intern(c->isolate, node_name, (uint32_t) strlen(node_name), 0);
    if (!str)
        return;
    XrValue name_val = xr_string_value(str);

    for (int i = 0; i < target_count; i++) {
        xr_channel_try_send(targets[i], name_val);
    }
#undef XR_MON_FIRE_MAX
}

/* ========== Remote Coroutine Monitor ========== */

XrChannel *xr_cluster_monitor_coro(XrayIsolate *X, const char *node_name, const char *coro_name) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !node_name || !coro_name)
        return NULL;

    // Find the target node
    XrClusterNode *node = xr_cluster_find_node(c, node_name);
    if (!node || node->state != XR_NODE_CONNECTED)
        return NULL;

    // Create notification channel
    XrChannel *ch = xr_channel_new(X, 1);
    if (!ch)
        return NULL;

    // Register in remote_coro_monitors list
    XrRemoteCoroMonitor *mon = (XrRemoteCoroMonitor *) xr_calloc(1, sizeof(XrRemoteCoroMonitor));
    if (!mon)
        return NULL;
    strncpy(mon->node_name, node_name, XR_NODE_NAME_MAX);
    strncpy(mon->coro_name, coro_name, XR_CORO_NAME_MAX);
    mon->notify_ch = ch;

    xr_mutex_lock(&c->monitors_lock);
    mon->next = c->remote_coro_monitors;
    c->remote_coro_monitors = mon;
    xr_mutex_unlock(&c->monitors_lock);

    // Send CORO_MONITOR frame to remote node
    uint8_t buf[256];
    int len = xr_frame_encode_coro_monitor(buf, sizeof(buf), XR_FRAME_CORO_MONITOR, coro_name);
    if (len > 0) {
        xr_cluster_node_enqueue(node, buf, (uint32_t) len);
    }

    return ch;
}

/* ========== CORO_MONITOR Forwarding ========== */

// Forwarding coroutine context: waits on local monitor channel,
// then sends CORO_EXIT frame to the remote node.
typedef struct {
    XrChannel *mon_ch;    // local monitor channel
    XrClusterNode *node;  // remote node to notify
    char coro_name[128];  // coroutine name
} XrCoroMonitorFwd;

static void coro_monitor_fwd_loop(void *arg) {
    XrCoroMonitorFwd *ctx = (XrCoroMonitorFwd *) arg;
    if (!ctx)
        return;

    // Block until the monitored coroutine exits
    XrValue reason_val;
    XrChanResult rr = xr_channel_recv(ctx->mon_ch, &reason_val, NULL);

    const char *reason = "normal";
    if (rr == XR_CHAN_OK && XR_IS_STRING(reason_val)) {
        reason = XR_TO_STRING(reason_val)->data;
    }

    // Send CORO_EXIT frame to the requesting remote node
    if (ctx->node && ctx->node->state == XR_NODE_CONNECTED) {
        uint8_t buf[256];
        int len = xr_frame_encode_coro_exit(buf, sizeof(buf), ctx->coro_name, reason);
        if (len > 0) {
            xr_cluster_node_enqueue(ctx->node, buf, (uint32_t) len);
        }
    }

    xr_free(ctx);
}

void xr_cluster_handle_coro_monitor(XrCluster *c, XrClusterNode *node, const char *coro_name) {
    // Remote node wants to monitor a local coroutine.
    // Register a local monitor; on exit, send CORO_EXIT frame back.
    if (!c || !c->isolate)
        return;

    XrCoroState *sched = (XrCoroState *) c->isolate->vm.coro_state;
    if (!sched || !sched->coro_registry)
        return;

    // Use the local registry monitor mechanism
    XrChannel *mon_ch = xr_coro_monitor(c->isolate, sched->coro_registry, coro_name);
    if (!mon_ch) {
        // Could not set up monitor — send immediate CORO_EXIT with "noproc"
        uint8_t buf[256];
        int len = xr_frame_encode_coro_exit(buf, sizeof(buf), coro_name, "noproc");
        if (len > 0) {
            xr_cluster_node_enqueue(node, buf, (uint32_t) len);
        }
        return;
    }

    // Create a forwarding coroutine that blocks on mon_ch and sends
    // CORO_EXIT frame when the monitored coroutine terminates.
    XrCoroMonitorFwd *ctx = (XrCoroMonitorFwd *) xr_malloc(sizeof(XrCoroMonitorFwd));
    if (!ctx)
        return;
    ctx->mon_ch = mon_ch;
    ctx->node = node;
    strncpy(ctx->coro_name, coro_name, sizeof(ctx->coro_name) - 1);
    ctx->coro_name[sizeof(ctx->coro_name) - 1] = '\0';

    XrCoroutine *fwd =
        xr_coro_create_native(c->isolate, coro_monitor_fwd_loop, ctx, "cluster_coro_fwd");
    if (fwd) {
        xr_coro_spawn(c->isolate, fwd);
    } else {
        xr_free(ctx);
    }
}

void xr_cluster_handle_coro_exit(XrCluster *c, const char *coro_name, const char *reason) {
    // Received CORO_EXIT from remote node — notify local monitors
    if (!c || !c->isolate)
        return;

    xr_mutex_lock(&c->monitors_lock);
    XrRemoteCoroMonitor **pp = &c->remote_coro_monitors;
    while (*pp) {
        XrRemoteCoroMonitor *mon = *pp;
        if (strcmp(mon->coro_name, coro_name) == 0) {
            // Send reason to local channel
            XrString *s = xr_string_new(c->isolate, reason, strlen(reason));
            XrValue val = s ? xr_string_value(s) : xr_null();
            xr_channel_try_send(mon->notify_ch, val);

            // Remove from list
            *pp = mon->next;
            xr_free(mon);
        } else {
            pp = &mon->next;
        }
    }
    xr_mutex_unlock(&c->monitors_lock);
}
