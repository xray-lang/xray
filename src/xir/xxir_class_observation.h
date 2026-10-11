/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_observation.h - Owned bounded records of physical class lifetimes
 *
 * KEY CONCEPT:
 *   Records outlive execution through an explicit domain lease, without callbacks.
 */
#ifndef XXIR_CLASS_OBSERVATION_H
#define XXIR_CLASS_OBSERVATION_H
#include "xxir_value.h"

typedef struct XrXirClassObservation XrXirClassObservation;
enum { XR_XIR_CLASS_OBSERVATION_CAPACITY = 128 };
typedef enum XrXirClassEventKind {
    XR_XIR_CLASS_CONSTRUCT = 1, XR_XIR_CLASS_FINALIZE, XR_XIR_CLASS_RECLAIM
} XrXirClassEventKind;
enum { XR_XIR_CLASS_EVENT_PARTIAL = 1, XR_XIR_CLASS_EVENT_RESIDUAL = 2 };
typedef struct XrXirClassEvent {
    uint64_t serial;
    uint32_t type, declaration, kind, flags, initialized_fields, status;
} XrXirClassEvent;

/* A serial is unique only within this live owned observation. Retain adds a
 * real domain lease; the last drop does not destroy an escaped class value. */
XR_FUNC XrXirValueStatus xr_xir_class_observation_retain(XrXirClassObservation *observation);
XR_FUNC void xr_xir_class_observation_drop(XrXirClassObservation **observation);
/* Start cursor at zero and count/overflow at zero/false. Only the exact current
 * cursor can consume records. Success advances it once; failure changes no
 * output. A nonempty drain consumes its prepaid copy; empty/repeated requests
 * spend ordinary domain work. Overflow is sticky and never discards old records. */
XR_FUNC XrXirValueStatus xr_xir_class_observation_drain(XrXirClassObservation *observation,
    uint64_t *cursor, XrXirClassEvent *events, uint32_t capacity, uint32_t *count, bool *overflow);
#endif // XXIR_CLASS_OBSERVATION_H
