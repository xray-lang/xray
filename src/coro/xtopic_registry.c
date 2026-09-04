/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtopic_registry.c - Synchronized channel registry for topic projections
 *
 * KEY CONCEPT:
 *   The registry is an allocation and synchronization provider, not the topic
 *   language owner. Its trie projects cluster.topicMatches for native loops
 *   that cannot enter Xray while a socket callback is active.
 */

#include "xtopic_registry.h"

#include "xchannel.h"
#include "xchannel_ops.h"
#include "../base/xmutex.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xbuffer.h"

#include <string.h>

typedef struct XrTopicSubscription {
    XrChannel *channel;
    struct XrTopicSubscription *next;
} XrTopicSubscription;

typedef struct XrTopicTrieNode XrTopicTrieNode;

typedef struct XrTopicTrieChild {
    char *segment;
    XrTopicTrieNode *node;
    struct XrTopicTrieChild *next;
} XrTopicTrieChild;

struct XrTopicTrieNode {
    XrTopicSubscription *exact_subscriptions;
    XrTopicSubscription *tail_subscriptions;
    XrTopicTrieNode *wildcard_child;
    XrTopicTrieChild *literal_children;
};

struct XrTopicRegistry {
    XrTopicTrieNode *root;
    int64_t subscription_count;
    uint32_t delivery_fanout_limit;
    XrAdaptiveMutex lock;
};

typedef struct XrTopicTargets {
    XrChannel **items;
    uint32_t count;
    uint32_t capacity;
    uint32_t limit;
    bool heap_allocated;
} XrTopicTargets;

bool xr_topic_name_valid(const char *topic) {
    if (!topic)
        return false;
    size_t length = strlen(topic);
    if (length == 0 || length > XR_TOPIC_PATTERN_MAX || topic[0] == '.' || topic[length - 1] == '.')
        return false;
    const char *segment = topic;
    for (const char *cursor = topic;; cursor++) {
        unsigned char byte = (unsigned char) *cursor;
        if (byte != '\0' && (byte < 0x21 || byte > 0x7e || byte == '*' || byte == '>'))
            return false;
        if (byte == '.' || byte == '\0') {
            if (cursor == segment)
                return false;
            if (byte == '\0')
                return true;
            segment = cursor + 1;
        }
    }
}

static bool topic_pattern_valid(const char *pattern) {
    if (!pattern)
        return false;
    size_t length = strlen(pattern);
    if (length == 0 || length > XR_TOPIC_PATTERN_MAX || pattern[0] == '.' ||
        pattern[length - 1] == '.')
        return false;
    const char *segment = pattern;
    for (const char *cursor = pattern;; cursor++) {
        unsigned char byte = (unsigned char) *cursor;
        if (byte != '\0' && (byte < 0x21 || byte > 0x7e))
            return false;
        if (byte == '.' || byte == '\0') {
            size_t segment_length = (size_t) (cursor - segment);
            if (segment_length == 0)
                return false;
            bool wildcard = segment_length == 1 && segment[0] == '*';
            bool tail = segment_length == 1 && segment[0] == '>';
            if (!wildcard && !tail &&
                (memchr(segment, '*', segment_length) || memchr(segment, '>', segment_length)))
                return false;
            if (tail && byte != '\0')
                return false;
            if (byte == '\0')
                return true;
            segment = cursor + 1;
        }
    }
}

bool xr_topic_pattern_matches(const char *pattern, const char *topic) {
    if (!topic_pattern_valid(pattern) || !xr_topic_name_valid(topic))
        return false;
    const char *pattern_cursor = pattern;
    const char *topic_cursor = topic;
    for (;;) {
        const char *pattern_end = strchr(pattern_cursor, '.');
        const char *topic_end = strchr(topic_cursor, '.');
        size_t pattern_length =
            pattern_end ? (size_t) (pattern_end - pattern_cursor) : strlen(pattern_cursor);
        size_t topic_length =
            topic_end ? (size_t) (topic_end - topic_cursor) : strlen(topic_cursor);
        bool tail = pattern_length == 1 && pattern_cursor[0] == '>';
        bool wildcard = pattern_length == 1 && pattern_cursor[0] == '*';
        if (tail)
            return topic_length > 0;
        if (!wildcard && (pattern_length != topic_length ||
                          memcmp(pattern_cursor, topic_cursor, pattern_length) != 0))
            return false;
        if (!pattern_end || !topic_end)
            return pattern_end == NULL && topic_end == NULL;
        pattern_cursor = pattern_end + 1;
        topic_cursor = topic_end + 1;
    }
}

static XrTopicTrieNode *topic_node_new(void) {
    return (XrTopicTrieNode *) xr_calloc(1, sizeof(XrTopicTrieNode));
}

static XrTopicTrieNode *topic_literal_child(XrTopicTrieNode *parent, const char *segment,
                                            size_t segment_length, bool create) {
    for (XrTopicTrieChild *child = parent->literal_children; child; child = child->next) {
        if (strlen(child->segment) == segment_length &&
            memcmp(child->segment, segment, segment_length) == 0)
            return child->node;
    }
    if (!create)
        return NULL;
    XrTopicTrieChild *child = (XrTopicTrieChild *) xr_calloc(1, sizeof(*child));
    if (!child)
        return NULL;
    child->segment = (char *) xr_malloc(segment_length + 1);
    if (!child->segment) {
        xr_free(child);
        return NULL;
    }
    memcpy(child->segment, segment, segment_length);
    child->segment[segment_length] = '\0';
    child->node = topic_node_new();
    if (!child->node) {
        xr_free(child->segment);
        xr_free(child);
        return NULL;
    }
    child->next = parent->literal_children;
    parent->literal_children = child;
    return child->node;
}

static bool topic_insert(XrTopicTrieNode *root, const char *pattern, XrTopicSubscription *sub) {
    XrTopicTrieNode *node = root;
    const char *cursor = pattern;
    while (*cursor) {
        const char *segment = cursor;
        while (*cursor && *cursor != '.')
            cursor++;
        size_t segment_length = (size_t) (cursor - segment);
        if (segment_length == 1 && segment[0] == '>') {
            if (*cursor != '\0')
                return false;
            sub->next = node->tail_subscriptions;
            node->tail_subscriptions = sub;
            return true;
        }
        if (segment_length == 1 && segment[0] == '*') {
            if (!node->wildcard_child)
                node->wildcard_child = topic_node_new();
            node = node->wildcard_child;
        } else {
            node = topic_literal_child(node, segment, segment_length, true);
        }
        if (!node)
            return false;
        if (*cursor == '.')
            cursor++;
    }
    sub->next = node->exact_subscriptions;
    node->exact_subscriptions = sub;
    return true;
}

static void topic_targets_append(XrTopicTargets *targets, XrTopicSubscription *subscriptions) {
    for (XrTopicSubscription *sub = subscriptions; sub; sub = sub->next) {
        if (!sub->channel || xr_channel_is_closed(sub->channel))
            continue;
        if (targets->count >= targets->limit)
            return;
        if (targets->count == targets->capacity) {
            uint32_t new_capacity = targets->capacity * 2;
            if (new_capacity > targets->limit)
                new_capacity = targets->limit;
            XrChannel **grown;
            if (targets->heap_allocated) {
                grown = (XrChannel **) xr_realloc(targets->items,
                                                  (size_t) new_capacity * sizeof(*grown));
            } else {
                grown = (XrChannel **) xr_malloc((size_t) new_capacity * sizeof(*grown));
                if (grown)
                    memcpy(grown, targets->items, (size_t) targets->count * sizeof(*grown));
            }
            if (!grown)
                return;
            targets->items = grown;
            targets->capacity = new_capacity;
            targets->heap_allocated = true;
        }
        targets->items[targets->count++] = sub->channel;
    }
}

static void topic_collect(XrTopicTrieNode *node, const char *topic, XrTopicTargets *targets) {
    if (!node || !topic || !*topic)
        return;
    const char *end = strchr(topic, '.');
    size_t segment_length = end ? (size_t) (end - topic) : strlen(topic);
    topic_targets_append(targets, node->tail_subscriptions);
    if (end) {
        XrTopicTrieNode *literal = topic_literal_child(node, topic, segment_length, false);
        if (literal)
            topic_collect(literal, end + 1, targets);
        if (node->wildcard_child)
            topic_collect(node->wildcard_child, end + 1, targets);
        return;
    }
    XrTopicTrieNode *literal = topic_literal_child(node, topic, segment_length, false);
    if (literal)
        topic_targets_append(targets, literal->exact_subscriptions);
    if (node->wildcard_child)
        topic_targets_append(targets, node->wildcard_child->exact_subscriptions);
}

static void topic_node_destroy(XrTopicTrieNode *node) {
    if (!node)
        return;
    XrTopicTrieChild *child = node->literal_children;
    while (child) {
        XrTopicTrieChild *next = child->next;
        topic_node_destroy(child->node);
        xr_free(child->segment);
        xr_free(child);
        child = next;
    }
    topic_node_destroy(node->wildcard_child);
    XrTopicSubscription *lists[2] = {node->exact_subscriptions, node->tail_subscriptions};
    for (size_t i = 0; i < 2; i++) {
        XrTopicSubscription *sub = lists[i];
        while (sub) {
            XrTopicSubscription *next = sub->next;
            if (sub->channel)
                xr_channel_close(sub->channel);
            xr_free(sub);
            sub = next;
        }
    }
    xr_free(node);
}

XrTopicRegistry *xr_topic_registry_new(uint32_t delivery_fanout_limit) {
    if (delivery_fanout_limit == 0)
        return NULL;
    XrTopicRegistry *registry = (XrTopicRegistry *) xr_calloc(1, sizeof(*registry));
    if (!registry)
        return NULL;
    registry->root = topic_node_new();
    if (!registry->root) {
        xr_free(registry);
        return NULL;
    }
    registry->delivery_fanout_limit = delivery_fanout_limit;
    xr_amutex_init(&registry->lock);
    return registry;
}

void xr_topic_registry_destroy(XrTopicRegistry *registry) {
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    XrTopicTrieNode *root = registry->root;
    registry->root = NULL;
    registry->subscription_count = 0;
    xr_amutex_unlock(&registry->lock);
    topic_node_destroy(root);
    xr_free(registry);
}

XrChannel *xr_topic_registry_subscribe(XrTopicRegistry *registry, XrVMRuntime *isolate,
                                       const char *pattern, uint32_t capacity) {
    if (!registry || !registry->root || !isolate || !topic_pattern_valid(pattern) || capacity == 0)
        return NULL;
    XrTopicSubscription *sub = (XrTopicSubscription *) xr_calloc(1, sizeof(*sub));
    if (!sub)
        return NULL;
    sub->channel = xr_channel_new_vm(isolate, capacity);
    if (!sub->channel) {
        xr_free(sub);
        return NULL;
    }
    xr_amutex_lock(&registry->lock);
    bool inserted = topic_insert(registry->root, pattern, sub);
    if (inserted)
        registry->subscription_count++;
    xr_amutex_unlock(&registry->lock);
    if (!inserted) {
        xr_channel_close(sub->channel);
        xr_free(sub);
        return NULL;
    }
    return sub->channel;
}

XrTopicDelivery xr_topic_registry_deliver(XrTopicRegistry *registry, XrVMRuntime *isolate,
                                          const char *topic, const uint8_t *payload,
                                          uint32_t payload_length) {
    if (!registry || !registry->root || !isolate || !topic || !payload)
        return XR_TOPIC_DELIVERY_UNAVAILABLE;
    enum {
        INLINE_TARGETS = 32
    };
    XrChannel *inline_targets[INLINE_TARGETS];
    XrTopicTargets targets = {
        .items = inline_targets,
        .count = 0,
        .capacity = registry->delivery_fanout_limit < INLINE_TARGETS
                        ? registry->delivery_fanout_limit
                        : INLINE_TARGETS,
        .limit = registry->delivery_fanout_limit,
        .heap_allocated = false,
    };
    xr_amutex_lock(&registry->lock);
    topic_collect(registry->root, topic, &targets);
    xr_amutex_unlock(&registry->lock);

    uint32_t delivered = 0;
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < targets.count; i++) {
        XrValue buffer = xr_buffer_copy_from_bytes(isolate, payload, payload_length);
        if (XR_IS_NULL(buffer)) {
            rejected++;
            continue;
        }
        if (xr_chan_try_send_transfer(isolate, targets.items[i], buffer, XR_TRANSFER_MOVE))
            delivered++;
        else
            rejected++;
    }
    if (targets.heap_allocated)
        xr_free(targets.items);
    if (delivered > 0)
        return XR_TOPIC_DELIVERY_ACCEPTED;
    if (rejected > 0)
        return XR_TOPIC_DELIVERY_OVERLOADED;
    return XR_TOPIC_DELIVERY_DISCONNECTED;
}

int64_t xr_topic_registry_count(XrTopicRegistry *registry) {
    if (!registry)
        return 0;
    xr_amutex_lock(&registry->lock);
    int64_t count = registry->subscription_count;
    xr_amutex_unlock(&registry->lock);
    return count;
}
