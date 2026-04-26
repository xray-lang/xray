/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_channel.c - Named Channel distributed routing
 *
 * KEY CONCEPT:
 *   Implements dist hooks for XrChannel. When a channel has dist != NULL,
 *   send/recv/close are routed through these hooks to the cluster layer.
 *
 *   Owner node: send/recv use local buffer (standard xchannel path)
 *   Proxy node: send serializes value and sends CHANNEL_SEND frame to Owner
 *               recv sends CHANNEL_RECV_REQ and waits for CHANNEL_RECV_RSP
 *
 * WHY THIS DESIGN:
 *   - Owner channels have dist != NULL but is_owner == true, so send/recv
 *     fall through to local buffer logic via returning XR_CHAN_OK after
 *     writing to local buffer directly.
 *   - Proxy channels serialize and forward over TCP.
 */

#include "cluster_channel.h"
#include "cluster.h"
#include "cluster_serial.h"
#include "cluster_proto.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/vm/xvm_internal.h"
#include "../../src/base/xchecks.h"

#include <stdlib.h>
#include <string.h>

/* ========== Dist Hook: send ========== */

static int dist_send(XrChannel *ch, XrValue value, XrCoroutine *coro) {
    (void) coro;
    if (!ch || !ch->dist)
        return XR_CHAN_CLOSED;

    XrDistChannel *dc = (XrDistChannel *) ch->dist;

    if (dc->is_owner) {
        // Owner: use local buffer (return a signal to fall through)
        // We return XR_CHAN_OK after directly doing local channel ops.
        // Actually, for Owner channels we should NOT intercept —
        // the dist hooks should not be called for Owner channels.
        // But as a safety measure, return -2 to signal "use local path".
        // The xchannel.c code casts our return to XrChanResult, so we
        // need to return a valid result. Let's just do local send inline.
        return -2;  // sentinel: caller should fall through to local logic
    }

    // Proxy: serialize value and send CHANNEL_SEND to Owner node
    XrCluster *c = dc->cluster;
    if (!c || !dc->owner_node || dc->owner_node->state != XR_NODE_CONNECTED)
        return XR_CHAN_CLOSED;

    XrSerialBuf sbuf;
    xr_serial_buf_init(&sbuf);
    if (xr_cluster_encode(c->isolate, value, &sbuf) != 0) {
        xr_serial_buf_free(&sbuf);
        return XR_CHAN_CLOSED;
    }

    // Build and enqueue CHANNEL_SEND frame via output queue
    size_t frame_size = 4 + 1 + 1 + strlen(dc->name) + sbuf.len;
    uint8_t stack_frame[4096];
    uint8_t *frame = (frame_size + 16 <= sizeof(stack_frame))
                         ? stack_frame
                         : (uint8_t *) xr_malloc(frame_size + 16);
    if (!frame) {
        xr_serial_buf_free(&sbuf);
        return XR_CHAN_CLOSED;
    }

    int flen = xr_frame_encode_channel_send(frame, frame_size + 16, dc->name, sbuf.data,
                                            (uint32_t) sbuf.len);
    xr_serial_buf_free(&sbuf);

    if (flen < 0) {
        if (frame != stack_frame)
            xr_free(frame);
        return XR_CHAN_CLOSED;
    }

    int rc = xr_cluster_node_enqueue(dc->owner_node, frame, (uint32_t) flen);
    if (frame != stack_frame)
        xr_free(frame);

    return (rc == 0) ? XR_CHAN_OK : XR_CHAN_CLOSED;
}

/* ========== Dist Hook: recv ========== */

static int dist_recv(XrChannel *ch, XrValue *out, XrCoroutine *coro) {
    (void) coro;
    if (!ch || !ch->dist || !out)
        return XR_CHAN_CLOSED;

    XrDistChannel *dc = (XrDistChannel *) ch->dist;

    if (dc->is_owner) {
        return -2;  // fall through to local logic
    }

    // Proxy: send CHANNEL_RECV_REQ to Owner via pending request table
    // No direct socket read — process_node dispatches CHANNEL_RECV_RSP
    XrCluster *c = dc->cluster;
    if (!c || !dc->owner_node || dc->owner_node->state != XR_NODE_CONNECTED)
        return XR_CHAN_CLOSED;

    // Allocate a request_id for this recv
    uint64_t req_id = atomic_fetch_add(&c->next_request_id, 1);
    uint64_t saved_req_id = req_id;  // save before serialization mutates it

    // Register pending request BEFORE sending
    XrChannel *rsp_ch =
        xr_cluster_node_add_pending(dc->owner_node, req_id, c->isolate, c->max_pending_requests);
    if (!rsp_ch)
        return XR_CHAN_FULL;

    // Build payload: [request_id 8B] [name_len 1B] [name ...]
    uint8_t name_len = (uint8_t) strlen(dc->name);
    uint8_t payload[256];
    // Big-endian request_id
    for (int i = 7; i >= 0; i--) {
        payload[i] = (uint8_t) (req_id & 0xFF);
        req_id >>= 8;
    }
    payload[8] = name_len;
    memcpy(payload + 9, dc->name, name_len);

    if (xr_cluster_node_send_frame(dc->owner_node, XR_FRAME_CHANNEL_RECV_REQ, payload,
                                   9 + name_len) != 0) {
        xr_cluster_node_take_pending(dc->owner_node, saved_req_id);
        return XR_CHAN_CLOSED;
    }

    // Block on temp Channel — process_node will deliver the result
    // Use blocking recv (yields coroutine until response arrives)
    XrChanResult rr = xr_channel_recv(rsp_ch, out, coro);
    if (rr != XR_CHAN_OK) {
        *out = xr_null();
        return XR_CHAN_CLOSED;
    }

    return XR_CHAN_OK;
}

/* ========== Dist Hook: try_send ========== */

static bool dist_try_send(XrChannel *ch, XrValue value) {
    if (!ch || !ch->dist)
        return false;
    XrDistChannel *dc = (XrDistChannel *) ch->dist;
    if (dc->is_owner)
        return false;  // fall through to local

    // For proxy, try_send is same as send but non-blocking intent
    // Since network I/O is involved, we just do regular send
    return dist_send(ch, value, NULL) == XR_CHAN_OK;
}

/* ========== Dist Hook: try_recv ========== */

static XrValue dist_try_recv(XrChannel *ch, bool *ok) {
    if (!ch || !ch->dist || !ok) {
        if (ok)
            *ok = false;
        return xr_null();
    }
    XrDistChannel *dc = (XrDistChannel *) ch->dist;
    if (dc->is_owner) {
        *ok = false;
        return xr_null();  // fall through to local
    }

    // Push model: check local buffer for data pushed by Owner
    if (ch->buf_count > 0) {
        return xr_channel_try_recv(ch, ok);
    }

    *ok = false;
    return xr_null();
}

/* ========== Dist Hook: close ========== */

static void dist_close(XrChannel *ch) {
    if (!ch || !ch->dist)
        return;
    XrDistChannel *dc = (XrDistChannel *) ch->dist;

    if (dc->is_owner) {
        // Owner closing: notify all connected nodes via output queues
        XrCluster *c = dc->cluster;
        if (!c)
            return;

        uint8_t frame[256];
        int flen = xr_frame_encode_channel_close(frame, sizeof(frame), dc->name);
        if (flen < 0)
            return;

        xr_mutex_lock(&c->nodes_lock);
        XrClusterNode *node = c->nodes;
        while (node) {
            if (node->state == XR_NODE_CONNECTED && node->conn) {
                xr_cluster_node_enqueue(node, frame, (uint32_t) flen);
            }
            node = node->next;
        }
        xr_mutex_unlock(&c->nodes_lock);
    } else {
        // Proxy closing: notify Owner via output queue
        if (dc->owner_node && dc->owner_node->state == XR_NODE_CONNECTED) {
            uint8_t frame[256];
            int flen = xr_frame_encode_channel_close(frame, sizeof(frame), dc->name);
            if (flen > 0) {
                xr_cluster_node_enqueue(dc->owner_node, frame, (uint32_t) flen);
            }
        }
    }

    // Unregister from cluster
    xr_cluster_unregister_channel(dc->cluster, dc->name);
}

/* ========== Dist Hook: destroy ========== */

static void dist_destroy(XrChannel *ch) {
    if (!ch || !ch->dist)
        return;
    // XrDistChannel is managed by the cluster registry,
    // unregister_channel will free it
    XrDistChannel *dc = (XrDistChannel *) ch->dist;
    xr_cluster_unregister_channel(dc->cluster, dc->name);
}

/* ========== Hook Table ========== */

static void dist_on_select_enter(XrChannel *ch) {
    xr_cluster_channel_subscribe(ch);
}

static void dist_on_select_exit(XrChannel *ch) {
    xr_cluster_channel_unsubscribe(ch);
}

static XrChannelDistHooks cluster_dist_hooks = {
    .send = dist_send,
    .recv = dist_recv,
    .try_send = dist_try_send,
    .try_recv = dist_try_recv,
    .close = dist_close,
    .destroy = dist_destroy,
    .on_select_enter = dist_on_select_enter,
    .on_select_exit = dist_on_select_exit,
};

void xr_cluster_channel_install_hooks(XrayIsolate *X) {
    if (!X)
        return;
    X->channel_dist_hooks = &cluster_dist_hooks;
}

void xr_cluster_channel_uninstall_hooks(XrayIsolate *X) {
    if (!X)
        return;
    X->channel_dist_hooks = NULL;
}

/* ========== Incoming Frame Handlers ========== */

/*
 * Deliver a remotely-sent value into an Owner channel's local buffer.
 *
 * Return codes map 1:1 to distinct failure modes so the proxy path
 * on the sending node can bubble a useful error to script code:
 *
 *   0              — enqueued successfully
 *  -1 (NO_CHANNEL) — no matching owner registration (lookup failure)
 *  -1 (DECODE)     — decode_value failed (bad wire frame)
 *   1 (FULL)       — channel buffer at capacity; the sender's
 *                    chan.send() should surface this as "buffer full"
 *                    rather than "closed". The proxy side reads this
 *                    and can (a) retry with backpressure, (b) fail
 *                    loudly, or (c) map to a distinct XR_CHAN_FULL
 *                    script-level code.
 *
 * Callers that only care about success/failure can keep treating
 * any non-zero return as an error.
 */
int xr_cluster_channel_handle_send(XrCluster *c, const char *channel_name,
                                   const uint8_t *value_data, uint32_t value_len) {
    if (!c)
        return -1;

    XrDistChannel *dc = xr_cluster_find_channel(c, channel_name);
    if (!dc || !dc->is_owner || !dc->channel)
        return -1;

    // Decode the value
    XrValue value;
    if (xr_cluster_decode_value(c->isolate, value_data, value_len, &value) != 0)
        return -1;

    // Write directly into the Owner channel's local buffer.
    // try_send failure here specifically means "buffer full" — the
    // channel is alive (we just looked it up) and xr_channel_try_send
    // does not fail for any other reason on a live channel. We
    // return +1 (not -1) so the caller can distinguish this from the
    // "unknown channel / decode error" cases above.
    XrChannel *ch = dc->channel;
    if (!xr_channel_try_send(ch, value)) {
        return 1;
    }

    // After writing to Owner buffer, push to subscribers if any
    if (dc->subscribers.count > 0) {
        xr_cluster_channel_push_to_subscribers(c, channel_name);
    }

    return 0;
}

/* ========== Push Model (for Proxy Channel select support) ========== */

/*
 * Push-to-subscribers delivery policy — AT-MOST-ONCE.
 *
 * This function pops one value from the Owner channel's local buffer
 * and forwards it to one round-robin-selected subscriber node. If
 * serialization fails mid-way, the value is DROPPED with no retry
 * and no indication to the original sender. This matches the
 * distributed-channel semantics documented in cluster.xr and is
 * consistent with Erlang / Go / NATS core pub-sub:
 *
 *   - A successful chan.send() on the sender means the owner's
 *     local buffer accepted the value — NOT that any subscriber
 *     received it.
 *
 *   - Subscriber-fanout failures (serialization OOM, transient
 *     network drop, subscriber dead) are silent. Script code that
 *     needs at-least-once semantics must layer its own ack on top
 *     (e.g. a response channel or a per-message sequence number).
 *
 * Callers that want to detect dropped pushes should monitor the
 * cluster_info() metrics counters (frames_sent vs subscriber count)
 * rather than the chan.send() return value.
 */
void xr_cluster_channel_push_to_subscribers(XrCluster *c, const char *name) {
    if (!c || !name)
        return;

    XrDistChannel *dc = xr_cluster_find_channel(c, name);
    if (!dc || !dc->is_owner || !dc->channel)
        return;
    if (dc->subscribers.count <= 0)
        return;

    // Try to read one item from Owner buffer
    bool ok = false;
    XrValue val = xr_channel_try_recv(dc->channel, &ok);
    if (!ok)
        return;

    // Serialize the value
    XrSerialBuf sbuf;
    xr_serial_buf_init(&sbuf);
    if (xr_cluster_encode(c->isolate, val, &sbuf) != 0) {
        xr_serial_buf_free(&sbuf);
        // Drop per at-most-once policy documented above — no retry,
        // no error surface. Callers that need at-least-once must
        // add their own ack / reply channel.
        return;
    }

    // Round-robin select one subscriber (O(1) array index)
    xr_mutex_lock(&c->channels_lock);
    XrClusterNode *target_node = NULL;
    if (dc->subscribers.count > 0) {
        int idx = dc->rr_index % dc->subscribers.count;
        dc->rr_index++;
        target_node = dc->subscribers.nodes[idx];
    }
    xr_mutex_unlock(&c->channels_lock);

    if (!target_node || target_node->state != XR_NODE_CONNECTED) {
        xr_serial_buf_free(&sbuf);
        return;
    }

    // Build and send CHANNEL_PUSH frame
    size_t frame_size = 4 + 1 + 1 + strlen(name) + sbuf.len;
    uint8_t stack_frame[4096];
    uint8_t *frame = (frame_size + 16 <= sizeof(stack_frame))
                         ? stack_frame
                         : (uint8_t *) xr_malloc(frame_size + 16);
    if (!frame) {
        xr_serial_buf_free(&sbuf);
        return;
    }

    int flen =
        xr_frame_encode_channel_push(frame, frame_size + 16, name, sbuf.data, (uint32_t) sbuf.len);
    xr_serial_buf_free(&sbuf);

    if (flen > 0) {
        xr_cluster_node_enqueue(target_node, frame, (uint32_t) flen);
    }
    if (frame != stack_frame)
        xr_free(frame);
}

int xr_cluster_channel_handle_push(XrCluster *c, const char *channel_name,
                                   const uint8_t *value_data, uint32_t value_len) {
    if (!c)
        return -1;

    XrDistChannel *dc = xr_cluster_find_channel(c, channel_name);
    if (!dc || dc->is_owner)
        return -1;

    // Proxy must have a local channel with buffer for pushed data
    if (!dc->channel)
        return -1;

    // Decode the value
    XrValue value;
    if (xr_cluster_decode_value(c->isolate, value_data, value_len, &value) != 0)
        return -1;

    // Write into Proxy channel's local buffer
    if (!xr_channel_try_send(dc->channel, value))
        return -1;

    // Wake select waiters
    xr_runtime_wake_channel(c->isolate, dc->channel, false);

    return 0;
}

void xr_cluster_channel_subscribe(XrChannel *ch) {
    if (!ch || !ch->dist)
        return;
    XrDistChannel *dc = (XrDistChannel *) ch->dist;
    if (dc->is_owner)
        return;  // Owner doesn't subscribe to itself

    XrCluster *c = dc->cluster;
    if (!c || !dc->owner_node || dc->owner_node->state != XR_NODE_CONNECTED)
        return;

    uint8_t frame[256];
    int flen = xr_frame_encode_channel_subscribe(frame, sizeof(frame), dc->name);
    if (flen > 0) {
        xr_cluster_node_enqueue(dc->owner_node, frame, (uint32_t) flen);
    }
}

void xr_cluster_channel_unsubscribe(XrChannel *ch) {
    if (!ch || !ch->dist)
        return;
    XrDistChannel *dc = (XrDistChannel *) ch->dist;
    if (dc->is_owner)
        return;

    XrCluster *c = dc->cluster;
    if (!c || !dc->owner_node || dc->owner_node->state != XR_NODE_CONNECTED)
        return;

    uint8_t frame[256];
    int flen = xr_frame_encode_channel_unsubscribe(frame, sizeof(frame), dc->name);
    if (flen > 0) {
        xr_cluster_node_enqueue(dc->owner_node, frame, (uint32_t) flen);
    }
}

void xr_cluster_channel_handle_close(XrCluster *c, const char *channel_name) {
    XrDistChannel *dc = xr_cluster_find_channel(c, channel_name);
    if (!dc || !dc->channel)
        return;

    // Close the local channel
    xr_channel_close(dc->channel);
}

void xr_cluster_channel_sync_to_node(XrCluster *c, XrClusterNode *node) {
    if (!c || !node || node->state != XR_NODE_CONNECTED)
        return;

    // Send CHANNEL_SYNC for each locally owned channel
    xr_mutex_lock(&c->channels_lock);
    for (int i = 0; i < XR_CLUSTER_CHANNEL_BUCKETS; i++) {
        XrDistChannel *dc = c->channel_buckets[i];
        while (dc) {
            if (dc->is_owner) {
                // Build CHANNEL_SYNC frame:
                // [name_len 1B] [name ...] [owner_name_len 1B] [owner_name ...] [buf_size 4B]
                uint8_t payload[512];
                uint8_t *p = payload;
                uint8_t name_len = (uint8_t) strlen(dc->name);
                *p++ = name_len;
                memcpy(p, dc->name, name_len);
                p += name_len;
                uint8_t owner_len = (uint8_t) strlen(c->self_name);
                *p++ = owner_len;
                memcpy(p, c->self_name, owner_len);
                p += owner_len;
                uint32_t buf_size = dc->channel ? dc->channel->buf_size : 0;
                p[0] = (uint8_t) (buf_size >> 24);
                p[1] = (uint8_t) (buf_size >> 16);
                p[2] = (uint8_t) (buf_size >> 8);
                p[3] = (uint8_t) (buf_size);
                p += 4;

                uint32_t payload_len = (uint32_t) (p - payload);
                xr_cluster_node_send_frame(node, XR_FRAME_CHANNEL_SYNC, payload, payload_len);
            }
            dc = dc->next;
        }
    }
    xr_mutex_unlock(&c->channels_lock);
}
