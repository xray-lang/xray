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

#include "cluster_internal.h"
#include "cluster_topic_core.h"
#include "../../src/runtime/object/xbuffer.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xchannel_ops.h"
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

/* ========== Listen ========== */

/*
 * What counts as a legal topic pattern, and what capacity a subscription may
 * ask for, are decided by listen() in stdlib/cluster/cluster.xr, which both
 * backends compile. They used to be decided three times -- here, in the VM
 * binding above, and again in xrt_cluster_listen -- and the three copies are
 * gone. What stays is the shape trie_insert needs to stay in bounds.
 */
struct XrChannel *cluster_transport_listen(XrVMRuntime *X, const char *pattern, uint32_t capacity) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !c->topic_root || !pattern || pattern[0] == '\0' ||
        strlen(pattern) > XR_TOPIC_PATTERN_MAX || capacity == 0)
        return NULL;

    XrTopicSubscription *sub = (XrTopicSubscription *) xr_calloc(1, sizeof(XrTopicSubscription));
    if (!sub)
        return NULL;

    strncpy(sub->pattern, pattern, XR_TOPIC_PATTERN_MAX);
    sub->pattern[XR_TOPIC_PATTERN_MAX] = '\0';

    // Buffered channel for receiving published values
    XrChannel *ch = xr_channel_new_vm(X, capacity);
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

    return ch;
}

/* ========== Deliver & Send ========== */

XrClusterDelivery cluster_transport_deliver_local(XrCluster *c, const char *topic,
                                                  const uint8_t *envelope, uint32_t envelope_len) {
    if (!c || !topic || !envelope || !c->topic_root)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;

    /*
     * Walk the trie under c->topics_lock to collect matching notify_ch
     * pointers into a stack-first / heap-fallback buffer. We release
     * the lock before the actual xr_channel_try_send calls — the send
     * path can wake select() waiters and re-enter cluster.send /
     * cluster.listen, which would recursively acquire topics_lock
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

    int delivered = 0;
    int rejected = 0;
    for (int i = 0; i < e.count; i++) {
        XrValue buffer = xr_buffer_copy_from_bytes(c->isolate, envelope, envelope_len);
        if (XR_IS_NULL(buffer)) {
            rejected++;
            continue;
        }
        if (xr_chan_try_send_transfer(c->isolate, e.targets[i], buffer, XR_TRANSFER_MOVE))
            delivered++;
        else
            rejected++;
    }

    if (e.grown_alloc)
        xr_free(e.targets);

    if (delivered > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (rejected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}

/* Build each peer's final wire frame once and transfer it directly into that
 * peer's output queue.  The previous path first allocated a shared transport
 * payload and then copied it into a second framed allocation per peer. */
static XrClusterDelivery transport_broadcast_envelope(XrCluster *c, XrClusterNode *exclude,
                                                      uint8_t hop_limit, const char *topic,
                                                      const uint8_t *envelope,
                                                      uint32_t envelope_len) {
    uint8_t topic_len = (uint8_t) strlen(topic);
    int connected = 0;
    int accepted = 0;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node != exclude && node->state == XR_NODE_CONNECTED) {
            connected++;
            if (cluster_node_send_transport_frame(node, hop_limit, topic, topic_len, envelope,
                                                  envelope_len) == 0)
                accepted++;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    if (accepted > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (connected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}

void cluster_transport_handle_frame(XrCluster *c, XrClusterNode *from, const char *topic,
                                    const uint8_t *envelope, uint32_t envelope_len,
                                    uint8_t hop_limit) {
    /*
     * A topic that arrived on the wire was never seen by cluster.xr -- the peer
     * that sent it may be running anything -- so the inbound path is the one
     * place the C side still asks whether a topic is well formed. The AOT
     * reader has always asked, through xr_cluster_topic_matches inside
     * aot_cluster_deliver_local; this path walked straight into the trie, so a
     * malformed wire topic could reach a subscriber on one backend and not on
     * the other. Both ask now, and they ask the same function.
     */
    if (!c || !topic || !envelope || envelope_len < XR_CLUSTER_ENVELOPE_HEADER_SIZE ||
        !xr_cluster_topic_valid(topic, false))
        return;

    (void) cluster_transport_deliver_local(c, topic, envelope, envelope_len);

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

    (void) transport_broadcast_envelope(c, from, (uint8_t) (hop_limit - 1), topic, envelope,
                                        envelope_len);
}

/*
 * Topic legality and the envelope floor are decided by send() in
 * stdlib/cluster/cluster.xr before this leaf is reached, so they are not
 * decided again here. What stays is the frame's own arithmetic: a topic plus
 * an envelope plus two length bytes has to fit in one payload, and that is a
 * fact about the wire, not a policy about the caller.
 */
XrClusterDelivery cluster_transport_send(XrVMRuntime *X, const char *topic, const uint8_t *envelope,
                                         uint32_t envelope_len, uint8_t hop_limit) {
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c || !atomic_load(&c->running))
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    size_t topic_len = strlen(topic);
    if (topic_len == 0 || topic_len > XR_TOPIC_PATTERN_MAX)
        return XR_CLUSTER_DELIVERY_INVALID_TOPIC;
    if (!envelope || envelope_len > XR_FRAME_MAX_PAYLOAD - 2 - topic_len)
        return XR_CLUSTER_DELIVERY_INVALID_ENVELOPE;

    XrClusterDelivery local = cluster_transport_deliver_local(c, topic, envelope, envelope_len);

    /*
     * Build the wire frame with the hop budget the caller was given. It is
     * TOPIC_DEFAULT_HOP_LIMIT in cluster.xr unless the caller named another,
     * and each downstream node decrements before forwarding further.
     */
    // Forward to all connected nodes (no split-horizon — we are the
    // origin, so every peer is a valid destination).
    XrClusterDelivery remote =
        transport_broadcast_envelope(c, NULL, hop_limit, topic, envelope, envelope_len);
    if (local == XR_CLUSTER_DELIVERY_ACCEPTED || remote == XR_CLUSTER_DELIVERY_ACCEPTED)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (local == XR_CLUSTER_DELIVERY_OVERLOADED || remote == XR_CLUSTER_DELIVERY_OVERLOADED)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}
