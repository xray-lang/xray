/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtopic_registry.h - Synchronized channel registry for topic projections
 *
 * KEY CONCEPT:
 *   Xray owns topic grammar and publication policy. This provider only keeps
 *   the synchronized channel index needed by native reader loops.
 */

#ifndef XTOPIC_REGISTRY_H
#define XTOPIC_REGISTRY_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct XrChannel;
struct XrVMRuntime;
typedef struct XrTopicRegistry XrTopicRegistry;

#define XR_TOPIC_PATTERN_MAX 127

typedef enum XrTopicDelivery {
    XR_TOPIC_DELIVERY_ACCEPTED = 0,
    XR_TOPIC_DELIVERY_UNAVAILABLE = 3,
    XR_TOPIC_DELIVERY_OVERLOADED = 4,
    XR_TOPIC_DELIVERY_DISCONNECTED = 5,
} XrTopicDelivery;

XR_FUNC bool xr_topic_name_valid(const char *topic);
XR_FUNC bool xr_topic_pattern_matches(const char *pattern, const char *topic);

XR_FUNC XrTopicRegistry *xr_topic_registry_new(uint32_t delivery_fanout_limit);
XR_FUNC void xr_topic_registry_destroy(XrTopicRegistry *registry);
XR_FUNC struct XrChannel *xr_topic_registry_subscribe(XrTopicRegistry *registry,
                                                      struct XrVMRuntime *isolate,
                                                      const char *pattern, uint32_t capacity);
XR_FUNC XrTopicDelivery xr_topic_registry_deliver(XrTopicRegistry *registry,
                                                  struct XrVMRuntime *isolate, const char *topic,
                                                  const uint8_t *payload, uint32_t payload_length);
XR_FUNC int64_t xr_topic_registry_count(XrTopicRegistry *registry);

#endif  // XTOPIC_REGISTRY_H
