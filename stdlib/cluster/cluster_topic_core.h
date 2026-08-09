/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_topic_core.h - Backend-neutral cluster topic rules
 *
 * KEY CONCEPT:
 *   VM and AOT transports share validation and wildcard matching exactly.
 *   Storage indexes remain backend adapters; the observable topic language
 *   has one owner here.
 */

#ifndef CLUSTER_TOPIC_CORE_H
#define CLUSTER_TOPIC_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define XR_CLUSTER_TOPIC_PATTERN_MAX 127

static inline bool xr_cluster_topic_valid(const char *text, bool pattern) {
    if (!text)
        return false;
    size_t length = strlen(text);
    if (length == 0 || length > XR_CLUSTER_TOPIC_PATTERN_MAX || text[0] == '.' ||
        text[length - 1] == '.')
        return false;
    const char *segment = text;
    for (const char *p = text;; p++) {
        unsigned char c = (unsigned char) *p;
        if (c != '\0' && (c < 0x21 || c > 0x7e))
            return false;
        if (c == '.' || c == '\0') {
            size_t segment_length = (size_t) (p - segment);
            if (segment_length == 0)
                return false;
            if (!pattern &&
                (memchr(segment, '*', segment_length) || memchr(segment, '>', segment_length)))
                return false;
            if (pattern) {
                bool star = segment_length == 1 && segment[0] == '*';
                bool tail = segment_length == 1 && segment[0] == '>';
                if (!star && !tail &&
                    (memchr(segment, '*', segment_length) || memchr(segment, '>', segment_length)))
                    return false;
                if (tail && c != '\0')
                    return false;
            }
            if (c == '\0')
                break;
            segment = p + 1;
        }
    }
    return true;
}

static inline bool xr_cluster_topic_segment_equal(const char *left, size_t left_length,
                                                  const char *right, size_t right_length) {
    return left_length == right_length && memcmp(left, right, left_length) == 0;
}

static inline bool xr_cluster_topic_matches(const char *pattern, const char *topic) {
    if (!xr_cluster_topic_valid(pattern, true) || !xr_cluster_topic_valid(topic, false))
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
        bool star = pattern_length == 1 && pattern_cursor[0] == '*';

        if (tail)
            return topic_length > 0;
        if (!star && !xr_cluster_topic_segment_equal(pattern_cursor, pattern_length, topic_cursor,
                                                     topic_length))
            return false;
        if (!pattern_end || !topic_end)
            return pattern_end == NULL && topic_end == NULL;
        pattern_cursor = pattern_end + 1;
        topic_cursor = topic_end + 1;
    }
}

#endif  // CLUSTER_TOPIC_CORE_H
