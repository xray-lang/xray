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
 *   Subscriptions are indexed by a segment trie so that publish(topic)
 *   costs O(topic_depth * branching) instead of O(total_subscriptions).
 *   The trie stores subs at the node for their final segment; wildcard
 *   children handle "*" (single segment) and a per-node gt_subs list
 *   handles ">" (remaining segments).
 *
 * WILDCARD SEMANTICS (match NATS exactly):
 *
 *   "*"  — matches EXACTLY ONE segment.
 *            "events.*"   matches "events.user", "events.click"
 *            "events.*"   does NOT match "events" (too few segments)
 *            "events.*"   does NOT match "events.user.login" (too many)
 *
 *   ">"  — matches ONE OR MORE remaining segments (trailing only).
 *            "events.>"   matches "events.user", "events.user.login"
 *            "events.>"   does NOT match "events" (requires >= 1 more
 *                         segment — this is the subtle rule that
 *                         surprises users familiar with MQTT's "#"
 *                         wildcard, which matches zero-or-more)
 *
 *   Mixed wildcards in one pattern are legal; ">" must be the final
 *   token (the parser rejects patterns where ">" is not last).
 *
 *   Segment separator is '.'. Empty segments ("a..b") are accepted by
 *   the parser but unlikely to match real topics; avoid them.
 */

#include "cluster.h"
#include "cluster_serial.h"
#include "cluster_node.h"
#include "../../src/coro/xchannel.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/base/xhash.h"
#include "../../src/base/xmalloc.h"

#include <string.h>

/* ========== Topic Trie ==========
 *
 * A topic pattern like "events.user.*" becomes a chain of nodes:
 *
 *   root --children[events]--> N1 --children[user]--> N2 --star_child--> N3
 *
 * with the subscription attached to N3->exact_subs. "events.>" attaches to
 * N1->gt_subs (matches any topic whose first segment is "events" and has
 * at least one more segment).
 *
 * Matching a topic walks every valid path: literal child first, then
 * star_child; each ancestor's gt_subs contributes if at least one more
 * segment remains. The tree is mutated under c->topics_lock; deliveries
 * collect target channels inside the lock and do the try_send outside.
 */

typedef struct XrTopicTrieChild {
    char *seg;  // owned literal segment
    struct XrTopicTrieNode *node;
    struct XrTopicTrieChild *next;
} XrTopicTrieChild;

typedef struct XrTopicTrieNode {
    XrTopicSubscription *exact_subs;     // subs that terminate at this node
    XrTopicSubscription *gt_subs;        // subs with ">" trailing here
    struct XrTopicTrieNode *star_child;  // "*" wildcard child
    XrTopicTrieChild *children;          // literal-segment children chain
} XrTopicTrieNode;

static XrTopicTrieNode *trie_node_new(void) {
    return (XrTopicTrieNode *) xr_calloc(1, sizeof(XrTopicTrieNode));
}

/*
 * Find or create a literal-segment child. Returns NULL on OOM. The
 * linked-list children layout trades O(k) child lookup for O(1) growth
 * — k is small in practice (per-level fan-out of real topic
 * hierarchies is a few to a few dozen), and a hash table would cost
 * more in cache misses than it saves for that range.
 */
static XrTopicTrieNode *trie_child_get_or_create(XrTopicTrieNode *parent, const char *seg,
                                                 size_t seglen) {
    XrTopicTrieChild *ch = parent->children;
    while (ch) {
        if (strlen(ch->seg) == seglen && memcmp(ch->seg, seg, seglen) == 0)
            return ch->node;
        ch = ch->next;
    }
    XrTopicTrieChild *newc = (XrTopicTrieChild *) xr_calloc(1, sizeof(XrTopicTrieChild));
    if (!newc)
        return NULL;
    newc->seg = (char *) xr_malloc(seglen + 1);
    if (!newc->seg) {
        xr_free(newc);
        return NULL;
    }
    memcpy(newc->seg, seg, seglen);
    newc->seg[seglen] = '\0';
    newc->node = trie_node_new();
    if (!newc->node) {
        xr_free(newc->seg);
        xr_free(newc);
        return NULL;
    }
    newc->next = parent->children;
    parent->children = newc;
    return newc->node;
}

static XrTopicTrieNode *trie_child_lookup(const XrTopicTrieNode *parent, const char *seg,
                                          size_t seglen) {
    XrTopicTrieChild *ch = parent->children;
    while (ch) {
        if (strlen(ch->seg) == seglen && memcmp(ch->seg, seg, seglen) == 0)
            return ch->node;
        ch = ch->next;
    }
    return NULL;
}

/*
 * Insert a subscription into the trie under `pattern`. On success `sub`
 * is owned by the trie (attached to exact_subs / gt_subs) and its next
 * pointer is overwritten. Returns 0 on success, -1 on allocation
 * failure (caller must then free `sub` itself).
 */
static int trie_insert(XrTopicTrieNode *root, const char *pattern, XrTopicSubscription *sub) {
    XrTopicTrieNode *cur = root;
    const char *p = pattern;
    while (*p) {
        const char *start = p;
        while (*p && *p != '.')
            p++;
        size_t seglen = (size_t) (p - start);

        if (seglen == 1 && start[0] == '>') {
            /* ">" terminates the pattern and attaches to gt_subs at the
             * current node. If anything follows it, that is a malformed
             * pattern; NATS rejects it so we do too. */
            if (*p != '\0')
                return -1;
            sub->next = cur->gt_subs;
            cur->gt_subs = sub;
            return 0;
        }

        XrTopicTrieNode *next;
        if (seglen == 1 && start[0] == '*') {
            if (!cur->star_child) {
                cur->star_child = trie_node_new();
                if (!cur->star_child)
                    return -1;
            }
            next = cur->star_child;
        } else {
            next = trie_child_get_or_create(cur, start, seglen);
            if (!next)
                return -1;
        }
        cur = next;
        if (*p == '.')
            p++;
    }
    sub->next = cur->exact_subs;
    cur->exact_subs = sub;
    return 0;
}

/*
 * Collect every subscription whose pattern matches `topic` into the
 * caller-supplied target buffer. `emit` handles bounds + dedup-free
 * append with a dynamic grow, matching the old deliver_local()
 * semantics. `has_more` says whether more topic segments remain past
 * `segstart` — used to decide if gt_subs at the current node matches
 * (">" requires at least one remaining segment).
 */
typedef struct {
    struct XrChannel **targets;
    int count;
    int cap;
    int hard_cap;
    struct XrChannel **inline_buf;
    int inline_cap;
    bool grown_alloc;
} XrTopicEmit;

static void emit_subs(XrTopicEmit *e, XrTopicSubscription *subs) {
    while (subs) {
        struct XrChannel *ch = subs->notify_ch;
        if (ch && !xr_channel_is_closed(ch)) {
            if (e->count >= e->hard_cap)
                return; /* drop overflow */
            if (e->count >= e->cap) {
                int new_cap = e->cap * 2;
                if (new_cap > e->hard_cap)
                    new_cap = e->hard_cap;
                struct XrChannel **grown;
                if (!e->grown_alloc) {
                    grown = (struct XrChannel **) xr_malloc((size_t) new_cap * sizeof(*grown));
                    if (grown)
                        memcpy(grown, e->targets, (size_t) e->count * sizeof(*grown));
                    e->grown_alloc = true;
                } else {
                    grown = (struct XrChannel **) xr_realloc(e->targets,
                                                             (size_t) new_cap * sizeof(*grown));
                }
                if (!grown)
                    return;
                e->targets = grown;
                e->cap = new_cap;
            }
            e->targets[e->count++] = ch;
        }
        subs = subs->next;
    }
}

static void trie_match(XrTopicTrieNode *node, const char *topic, XrTopicEmit *e) {
    if (!node)
        return;
    const char *p = topic;
    /* The current segment bounds run from `p` to the next '.' (or end). */
    const char *seg_start = p;
    while (*p && *p != '.')
        p++;
    size_t seglen = (size_t) (p - seg_start);
    bool more_segments = (*p == '.');

    /* ">" at this node matches if we have at least the current segment,
     * which is always true when we are invoked with a non-empty topic. */
    if (seglen > 0)
        emit_subs(e, node->gt_subs);

    if (seglen == 0) {
        /* Degenerate: empty topic or trailing dot. Treat as no match. */
        return;
    }

    const char *rest = more_segments ? p + 1 : p; /* next-segment start or '\0' */

    if (more_segments) {
        XrTopicTrieNode *lit = trie_child_lookup(node, seg_start, seglen);
        if (lit)
            trie_match(lit, rest, e);
        if (node->star_child)
            trie_match(node->star_child, rest, e);
    } else {
        /* Final segment: collect exact terminators from both literal and
         * "*" children. */
        XrTopicTrieNode *lit = trie_child_lookup(node, seg_start, seglen);
        if (lit)
            emit_subs(e, lit->exact_subs);
        if (node->star_child)
            emit_subs(e, node->star_child->exact_subs);
    }
}

static void trie_destroy(XrTopicTrieNode *node) {
    if (!node)
        return;
    XrTopicTrieChild *ch = node->children;
    while (ch) {
        XrTopicTrieChild *next = ch->next;
        trie_destroy(ch->node);
        xr_free(ch->seg);
        xr_free(ch);
        ch = next;
    }
    trie_destroy(node->star_child);
    /* exact_subs / gt_subs structs are owned by the trie; release them.
     * The channels they hold are closed by the caller (topics_destroy)
     * so we just free the XrTopicSubscription memory here. */
    XrTopicSubscription *s = node->exact_subs;
    while (s) {
        XrTopicSubscription *n = s->next;
        xr_free(s);
        s = n;
    }
    s = node->gt_subs;
    while (s) {
        XrTopicSubscription *n = s->next;
        xr_free(s);
        s = n;
    }
    xr_free(node);
}

/* ========== Internal Trie Lifecycle ========== */

int cluster_topics_init(XrCluster *c) {
    if (!c)
        return -1;
    c->topic_root = trie_node_new();
    return c->topic_root ? 0 : -1;
}

void cluster_topics_destroy(XrCluster *c) {
    if (!c)
        return;
    /* Close every subscriber channel first so consumers unblock, then
     * tear the tree down. Doing it in two passes keeps the delicate
     * re-entrancy rules of xr_channel_close (can wake other coros)
     * away from the allocator churn. */
    xr_amutex_lock(&c->topics_lock);
    XrTopicTrieNode *root = c->topic_root;
    c->topic_root = NULL;
    c->topic_sub_count = 0;
    xr_amutex_unlock(&c->topics_lock);

    if (root) {
        /*
         * Close every subscriber channel first so waiters unblock, then
         * recursively free the tree. We use an explicit 64-entry stack
         * instead of recursion because the trie depth is bounded by
         * XR_TOPIC_PATTERN_MAX (127 chars, realistic depth < 32), and a
         * stack keeps the teardown path allocation-free even in OOM.
         */
        XrTopicTrieNode *stack[64];
        int sp = 0;
        stack[sp++] = root;
        while (sp > 0) {
            XrTopicTrieNode *n = stack[--sp];
            for (XrTopicSubscription *s = n->exact_subs; s; s = s->next)
                if (s->notify_ch)
                    xr_channel_close(s->notify_ch);
            for (XrTopicSubscription *s = n->gt_subs; s; s = s->next)
                if (s->notify_ch)
                    xr_channel_close(s->notify_ch);
            if (n->star_child && sp < (int) (sizeof(stack) / sizeof(stack[0])))
                stack[sp++] = n->star_child;
            for (XrTopicTrieChild *c2 = n->children;
                 c2 && sp < (int) (sizeof(stack) / sizeof(stack[0])); c2 = c2->next)
                stack[sp++] = c2->node;
        }
        trie_destroy(root);
    }
}

/* ========== Subscribe ========== */

struct XrChannel *xr_cluster_topic_subscribe(XrVMRuntime *X, const char *pattern) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !pattern || !c->topic_root)
        return NULL;

    XrTopicSubscription *sub = (XrTopicSubscription *) xr_calloc(1, sizeof(XrTopicSubscription));
    if (!sub)
        return NULL;

    strncpy(sub->pattern, pattern, XR_TOPIC_PATTERN_MAX);
    sub->pattern[XR_TOPIC_PATTERN_MAX] = '\0';

    // Buffered channel for receiving published values
    XrChannel *ch = xr_channel_new_vm(X, 64);
    if (!ch) {
        xr_free(sub);
        return NULL;
    }
    sub->notify_ch = ch;

    xr_amutex_lock(&c->topics_lock);
    int rc = trie_insert(c->topic_root, sub->pattern, sub);
    if (rc == 0)
        c->topic_sub_count++;
    xr_amutex_unlock(&c->topics_lock);

    if (rc != 0) {
        /* Malformed pattern (">" in the middle) or OOM — back out the
         * channel + struct. xr_channel_close is safe here because no
         * other coro has a handle to this channel yet. */
        xr_channel_close(ch);
        xr_free(sub);
        return NULL;
    }

    // Broadcast subscription to all connected nodes
    uint8_t name_len = (uint8_t) strlen(pattern);
    uint8_t payload[256];
    payload[0] = name_len;
    memcpy(payload + 1, pattern, name_len);

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED) {
            xr_cluster_node_send_frame(node, XR_FRAME_TOPIC_SUBSCRIBE, payload, 1 + name_len);
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    return ch;
}

/* ========== Deliver & Publish ========== */

void xr_cluster_topic_deliver_local(XrCluster *c, const char *topic, XrValue value) {
    if (!c || !topic || !c->topic_root)
        return;

    /*
     * Walk the trie under c->topics_lock to collect matching notify_ch
     * pointers into a stack-first / heap-fallback buffer. We release
     * the lock before the actual xr_channel_try_send calls — the send
     * path can wake select() waiters and re-enter cluster.publish /
     * cluster.subscribe, which would recursively acquire topics_lock
     * and deadlock.
     *
     * Budget: 256 matches per publish is plenty for typical topologies;
     * overflow is silently dropped to preserve at-most-once delivery.
     */
    enum {
        INLINE_CAP = 32,
        HARD_CAP = 256
    };
    struct XrChannel *inline_buf[INLINE_CAP];
    XrTopicEmit e = {
        .targets = inline_buf,
        .count = 0,
        .cap = INLINE_CAP,
        .hard_cap = HARD_CAP,
        .inline_buf = inline_buf,
        .inline_cap = INLINE_CAP,
        .grown_alloc = false,
    };

    xr_amutex_lock(&c->topics_lock);
    trie_match(c->topic_root, topic, &e);
    xr_amutex_unlock(&c->topics_lock);

    for (int i = 0; i < e.count; i++) {
        // Channel may have been closed by another thread between
        // collection and delivery; try_send handles closed channels
        // gracefully.
        xr_channel_try_send(e.targets[i], value);
    }

    if (e.grown_alloc)
        xr_free(e.targets);
}

/*
 * Build the wire-format payload shared by both the local publish
 * path (xr_cluster_topic_publish) and the forwarding path
 * (xr_cluster_topic_handle_publish). Layout:
 *
 *   [hop_limit 1B] [topic_len 1B] [topic ...] [value_data ...]
 *
 * Returns 0 on success, -1 on alloc failure. On success the caller
 * owns fb and must free with xr_frame_buf_free.
 */
static int topic_build_publish_frame(XrVMRuntime *X, const char *topic, const XrValue *value,
                                     uint8_t hop_limit, XrFrameBuf *fb_out) {
    if (!topic || !value || !fb_out)
        return -1;

    XrSerialBuf sbuf;
    cluster_serial_buf_init(&sbuf);
    if (cluster_encode(X, *value, &sbuf) != 0) {
        cluster_serial_buf_free(&sbuf);
        return -1;
    }

    uint8_t topic_len = (uint8_t) strlen(topic);
    uint32_t payload_len = 2 + topic_len + (uint32_t) sbuf.len;
    xr_frame_buf_init(fb_out, payload_len);
    if (!fb_out->data) {
        cluster_serial_buf_free(&sbuf);
        return -1;
    }
    fb_out->data[0] = hop_limit;
    fb_out->data[1] = topic_len;
    memcpy(fb_out->data + 2, topic, topic_len);
    memcpy(fb_out->data + 2 + topic_len, sbuf.data, sbuf.len);
    cluster_serial_buf_free(&sbuf);

    return (int) payload_len;
}

/*
 * Send the already-built TOPIC_PUBLISH frame to every connected peer
 * except `exclude` (used for split-horizon forwarding). Caller owns
 * the XrFrameBuf and is responsible for freeing it.
 */
static void topic_broadcast_frame(XrCluster *c, XrClusterNode *exclude, const uint8_t *payload,
                                  uint32_t payload_len) {
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node != exclude && node->state == XR_NODE_CONNECTED) {
            xr_cluster_node_send_frame(node, XR_FRAME_TOPIC_PUBLISH, payload, payload_len);
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
}

void xr_cluster_topic_handle_publish(XrCluster *c, XrClusterNode *from, const char *topic,
                                     const uint8_t *value_data, uint32_t value_len,
                                     uint8_t hop_limit) {
    if (!c || !topic)
        return;

    // Decode the value
    XrValue value;
    if (cluster_decode_value(c->isolate, value_data, value_len, &value) != 0)
        return;

    // Deliver to every matching local subscription — this happens
    // regardless of hop_limit because we are the intended recipient.
    xr_cluster_topic_deliver_local(c, topic, value);

    /*
     * Controlled flooding. If hop_limit == 0 the originator (or a
     * previous hop) has decided this frame should not propagate
     * further.
     *
     * Otherwise re-forward to every connected peer EXCEPT `from`
     * (split-horizon) with hop_limit - 1. This is NOT loop-free on
     * graphs with cycles longer than the hop limit, but three
     * things bound the damage:
     *
     *   1. Every hop decrements; after XR_TOPIC_DEFAULT_HOP_LIMIT
     *      hops the frame dies naturally.
     *   2. Split-horizon eliminates the 2-hop A→B→A loop entirely.
     *   3. Duplicate delivery on triangular meshes (A→B→C→A) is
     *      tolerated by design — subscribers see at-most the value
     *      a few times rather than unbounded times.
     *
     * A proper fix would cache a recent message-id set per cluster
     * and drop duplicates; tracked as a separate item.
     */
    if (hop_limit == 0)
        return;

    uint8_t next_hop = (uint8_t) (hop_limit - 1);

    // Re-serialize with the decremented hop byte. Rebuilding is cheap
    // compared to the encode that would otherwise be required.
    XrFrameBuf fb;
    int payload_len = topic_build_publish_frame(c->isolate, topic, &value, next_hop, &fb);
    if (payload_len < 0)
        return;

    topic_broadcast_frame(c, from, fb.data, (uint32_t) payload_len);
    xr_frame_buf_free(&fb);
}

int xr_cluster_topic_publish(XrVMRuntime *X, const char *topic, XrValue value) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !topic)
        return -1;

    // Deliver to local subscribers first
    xr_cluster_topic_deliver_local(c, topic, value);

    /*
     * Build wire frame with the cluster-wide default hop limit. Each
     * downstream node decrements before forwarding further; see the
     * detailed comment on XR_TOPIC_DEFAULT_HOP_LIMIT in
     * cluster_proto.h for the depth-vs-damage trade-off.
     */
    XrFrameBuf fb;
    int payload_len = topic_build_publish_frame(X, topic, &value, XR_TOPIC_DEFAULT_HOP_LIMIT, &fb);
    if (payload_len < 0)
        return -1;

    // Forward to all connected nodes (no split-horizon — we are the
    // origin, so every peer is a valid destination).
    topic_broadcast_frame(c, NULL, fb.data, (uint32_t) payload_len);
    xr_frame_buf_free(&fb);
    return 0;
}
