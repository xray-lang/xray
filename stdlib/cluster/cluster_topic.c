/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_topic.c - Topic-based Pub/Sub with wildcard matching
 *
 * KEY CONCEPT:
 *   NATS-style topic matching with "*" (one segment) and ">" (remaining).
 *   Subscriptions are hashed by pattern; delivery uses fast exact-match
 *   lookup plus slow wildcard scan.
 */

#include "cluster.h"
#include "cluster_serial.h"
#include "cluster_node.h"
#include "../../src/coro/xchannel.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/base/xhash.h"
#include "../../src/base/xmalloc.h"

#include <string.h>

static inline uint32_t str_hash_topic(const char *s) {
    return xr_hash_bytes(s, strlen(s));
}

/* ========== Topic Matching ========== */

// NATS-style topic matching:
//   "*" matches exactly one segment (delimited by ".")
//   ">" matches one or more remaining segments
//   Exact segments must match literally
bool xr_topic_match(const char *pattern, const char *topic) {
    if (!pattern || !topic) return false;

    const char *p = pattern;
    const char *t = topic;

    while (*p && *t) {
        if (*p == '>') {
            // ">" matches all remaining segments
            return true;
        }
        if (*p == '*') {
            // "*" matches exactly one segment
            while (*t && *t != '.') t++;
            p++;
            if (*p == '.' && *t == '.') { p++; t++; continue; }
            if (*p == '\0' && *t == '\0') return true;
            if (*p == '\0' && *t == '.') return false;
            return (*p == '\0' && *t == '\0');
        }
        // Literal segment comparison
        while (*p && *p != '.' && *t && *t != '.') {
            if (*p != *t) return false;
            p++; t++;
        }
        if (*p == '.' && *t == '.') { p++; t++; continue; }
        if (*p == '\0' && *t == '\0') return true;
        break;
    }

    // Handle trailing ">" in pattern
    if (*p == '>' && *t == '\0') return false; // ">" needs at least one segment
    return (*p == '\0' && *t == '\0');
}

/* ========== Subscribe / Unsubscribe ========== */

struct XrChannel *xr_cluster_topic_subscribe(XrayIsolate *X,
                                              const char *pattern) {
    XrCluster *c = (XrCluster *)X->cluster;
    if (!c || !pattern) return NULL;

    XrTopicSubscription *sub = (XrTopicSubscription *)xr_calloc(1, sizeof(XrTopicSubscription));
    if (!sub) return NULL;

    strncpy(sub->pattern, pattern, XR_TOPIC_PATTERN_MAX);
    sub->pattern[XR_TOPIC_PATTERN_MAX] = '\0';

    // Buffered channel for receiving published values
    XrChannel *ch = xr_channel_new(X, 64);
    if (!ch) { xr_free(sub); return NULL; }
    sub->notify_ch = ch;

    uint32_t bucket = str_hash_topic(pattern) % XR_CLUSTER_TOPIC_BUCKETS;

    xr_spinlock_lock(&c->topics_lock);
    sub->next = c->topic_buckets[bucket];
    c->topic_buckets[bucket] = sub;
    c->topic_sub_count++;
    xr_spinlock_unlock(&c->topics_lock);

    // Broadcast subscription to all connected nodes
    uint8_t name_len = (uint8_t)strlen(pattern);
    uint8_t payload[256];
    payload[0] = name_len;
    memcpy(payload + 1, pattern, name_len);

    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED) {
            xr_cluster_node_send_frame(node, XR_FRAME_TOPIC_SUBSCRIBE,
                                        payload, 1 + name_len);
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);

    return ch;
}

void xr_cluster_topic_unsubscribe(XrCluster *c, const char *pattern) {
    if (!c || !pattern) return;

    uint32_t bucket = str_hash_topic(pattern) % XR_CLUSTER_TOPIC_BUCKETS;

    xr_spinlock_lock(&c->topics_lock);
    XrTopicSubscription **pp = &c->topic_buckets[bucket];
    while (*pp) {
        if (strcmp((*pp)->pattern, pattern) == 0) {
            XrTopicSubscription *found = *pp;
            *pp = found->next;
            c->topic_sub_count--;
            if (found->notify_ch) xr_channel_close(found->notify_ch);
            xr_free(found);
            break;
        }
        pp = &(*pp)->next;
    }
    xr_spinlock_unlock(&c->topics_lock);

    // Broadcast unsubscription
    uint8_t name_len = (uint8_t)strlen(pattern);
    uint8_t payload[256];
    payload[0] = name_len;
    memcpy(payload + 1, pattern, name_len);

    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED) {
            xr_cluster_node_send_frame(node, XR_FRAME_TOPIC_UNSUBSCRIBE,
                                        payload, 1 + name_len);
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);
}

/* ========== Deliver & Publish ========== */

void xr_cluster_topic_deliver_local(XrCluster *c, const char *topic, XrValue value) {
    if (!c || !topic) return;

    /*
     * Collect matching notify_ch pointers under the lock, deliver outside.
     *
     * Why: xr_channel_try_send wakes select() waiters, which may run user
     * callbacks that re-enter cluster.publish / cluster.subscribe. If we
     * called try_send under topics_lock, that re-entry would recursively
     * acquire the same lock -> deadlock (spinlock) or corruption.
     *
     * Budget: 256 matches per publish is plenty for typical topologies;
     * overflow is silently dropped (at-most-once semantics).
     */
    #define XR_TOPIC_DELIVER_INLINE 32
    #define XR_TOPIC_DELIVER_MAX    256
    struct XrChannel *inline_targets[XR_TOPIC_DELIVER_INLINE];
    struct XrChannel **targets = inline_targets;
    int target_count = 0;
    int target_cap = XR_TOPIC_DELIVER_INLINE;

    xr_spinlock_lock(&c->topics_lock);

    // Fast path: exact match via hash lookup
    uint32_t exact_bucket = str_hash_topic(topic) % XR_CLUSTER_TOPIC_BUCKETS;
    XrTopicSubscription *sub = c->topic_buckets[exact_bucket];
    while (sub) {
        if (strcmp(sub->pattern, topic) == 0 &&
            sub->notify_ch && !xr_channel_is_closed(sub->notify_ch)) {
            if (target_count >= target_cap) {
                if (target_cap >= XR_TOPIC_DELIVER_MAX) goto collect_done;
                int new_cap = target_cap * 2;
                if (new_cap > XR_TOPIC_DELIVER_MAX) new_cap = XR_TOPIC_DELIVER_MAX;
                struct XrChannel **grown;
                if (targets == inline_targets) {
                    grown = (struct XrChannel **)xr_malloc(
                        (size_t)new_cap * sizeof(*grown));
                    if (grown) memcpy(grown, targets,
                                      (size_t)target_count * sizeof(*grown));
                } else {
                    grown = (struct XrChannel **)xr_realloc(
                        targets, (size_t)new_cap * sizeof(*grown));
                }
                if (!grown) goto collect_done;
                targets = grown;
                target_cap = new_cap;
            }
            targets[target_count++] = sub->notify_ch;
        }
        sub = sub->next;
    }

    // Slow path: scan all buckets for wildcard patterns only
    for (int i = 0; i < XR_CLUSTER_TOPIC_BUCKETS; i++) {
        sub = c->topic_buckets[i];
        while (sub) {
            // Skip exact patterns (already handled above)
            bool has_wildcard = (strchr(sub->pattern, '*') != NULL ||
                                 strchr(sub->pattern, '>') != NULL);
            if (has_wildcard && xr_topic_match(sub->pattern, topic) &&
                sub->notify_ch && !xr_channel_is_closed(sub->notify_ch)) {
                if (target_count >= target_cap) {
                    if (target_cap >= XR_TOPIC_DELIVER_MAX) goto collect_done;
                    int new_cap = target_cap * 2;
                    if (new_cap > XR_TOPIC_DELIVER_MAX) new_cap = XR_TOPIC_DELIVER_MAX;
                    struct XrChannel **grown;
                    if (targets == inline_targets) {
                        grown = (struct XrChannel **)xr_malloc(
                            (size_t)new_cap * sizeof(*grown));
                        if (grown) memcpy(grown, targets,
                                          (size_t)target_count * sizeof(*grown));
                    } else {
                        grown = (struct XrChannel **)xr_realloc(
                            targets, (size_t)new_cap * sizeof(*grown));
                    }
                    if (!grown) goto collect_done;
                    targets = grown;
                    target_cap = new_cap;
                }
                targets[target_count++] = sub->notify_ch;
            }
            sub = sub->next;
        }
    }

collect_done:
    xr_spinlock_unlock(&c->topics_lock);

    // Deliver to collected targets outside the lock. Safe now:
    // - try_send may wake select() waiters and trigger re-entry
    // - the subscription table can be freely mutated in response
    for (int i = 0; i < target_count; i++) {
        // Channel may have been closed by another thread between collection
        // and delivery; try_send handles closed channels gracefully.
        xr_channel_try_send(targets[i], value);
    }

    if (targets != inline_targets) xr_free(targets);
    #undef XR_TOPIC_DELIVER_INLINE
    #undef XR_TOPIC_DELIVER_MAX
}

void xr_cluster_topic_handle_publish(XrCluster *c, const char *topic,
                                      const uint8_t *value_data, uint32_t value_len) {
    if (!c || !topic) return;

    // Decode the value
    XrValue value;
    if (xr_cluster_decode_value(c->isolate, value_data, value_len, &value) != 0)
        return;

    // Deliver to local subscribers only (don't re-forward to avoid loops)
    xr_cluster_topic_deliver_local(c, topic, value);
}

int xr_cluster_topic_publish(XrayIsolate *X, const char *topic, XrValue value) {
    XrCluster *c = (XrCluster *)X->cluster;
    if (!c || !topic) return -1;

    // Deliver to local subscribers first
    xr_cluster_topic_deliver_local(c, topic, value);

    // Serialize value
    XrSerialBuf sbuf;
    xr_serial_buf_init(&sbuf);
    if (xr_cluster_encode(X, value, &sbuf) != 0) {
        xr_serial_buf_free(&sbuf);
        return -1;
    }

    // Build TOPIC_PUBLISH payload: [topic_len 1B] [topic ...] [value_data ...]
    uint8_t topic_len = (uint8_t)strlen(topic);
    uint32_t payload_len = 1 + topic_len + (uint32_t)sbuf.len;
    XrFrameBuf fb;
    xr_frame_buf_init(&fb, payload_len);
    if (!fb.data) {
        xr_serial_buf_free(&sbuf);
        return -1;
    }
    fb.data[0] = topic_len;
    memcpy(fb.data + 1, topic, topic_len);
    memcpy(fb.data + 1 + topic_len, sbuf.data, sbuf.len);
    xr_serial_buf_free(&sbuf);

    // Forward to all connected nodes
    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED) {
            xr_cluster_node_send_frame(node, XR_FRAME_TOPIC_PUBLISH,
                                        fb.data, payload_len);
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);

    xr_frame_buf_free(&fb);
    return 0;
}
