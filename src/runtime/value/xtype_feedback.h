/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_feedback.h - Runtime type feedback for profile-guided compilation
 *
 * KEY CONCEPT:
 *   Collects runtime type information at function call boundaries.
 *   Both JIT and future AOT compilers consume this data for specialization.
 *   Lightweight design: ~3-5 instructions per call, <2% overhead.
 *
 * LAYERING:
 *   Lives at runtime/value. It is a per-XrProto profile written by the VM
 *   and read by JIT/AOT consumers — strictly runtime data, never a jit-only
 *   concept. Placing it here eliminates the runtime -> jit upward include
 *   previously caused by xchunk.c destroying feedback from xm_feedback.h.
 *
 * WHY THIS DESIGN:
 *   - Does NOT depend on XRAY_HAS_JIT — always compiled, all platforms
 *   - Profile collection is a shared foundation; JIT/AOT are consumers
 *   - Bitmask accumulation (OR) instead of enum lattice (simpler than V8)
 *   - Stability detection via consecutive-stable sampling
 */

#ifndef XTYPE_FEEDBACK_H
#define XTYPE_FEEDBACK_H

#include "xvalue.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../base/xdefs.h"

/* ========== Type Feedback Flags (bitmask, OR-accumulated) ========== */

#define XFB_TYPE_NONE 0x00
#define XFB_TYPE_INT 0x01
#define XFB_TYPE_FLOAT 0x02
#define XFB_TYPE_BOOL 0x04
#define XFB_TYPE_STRING 0x08
#define XFB_TYPE_OBJECT 0x10
#define XFB_TYPE_NULL 0x20
#define XFB_TYPE_MIXED 0x3F  // multiple types seen → degrade to any

// Max parameters tracked per function
#define XFB_MAX_PARAMS 8

// Stability threshold: consecutive samples with no type change
#define XFB_STABLE_THRESHOLD 32

/* ========== Type Feedback Structure ========== */

/*
 * Per-function runtime type profile.
 * Lazily allocated when call_count exceeds XFB_ALLOC_THRESHOLD.
 * Stored in XrProto.type_feedback.
 */
typedef struct XmTypeFeedback {
    uint8_t arg_types[XFB_MAX_PARAMS];  // observed param types (OR-accumulated)
    uint8_t return_type;                // observed return type (OR-accumulated)
    uint32_t sample_count;              // total samples collected
    uint16_t stable_count;              // consecutive samples with no change
    bool stable;                        // true if profile is considered stable
} XmTypeFeedback;

// Threshold: start collecting profile after this many calls
#define XFB_ALLOC_THRESHOLD 10

/* ========== Inline Helpers ========== */

// Convert an XrValue tag to a feedback type flag
static inline uint8_t xfb_value_type_flag(XrValue val) {
    uint32_t tag = val.tag;
    switch (tag) {
        case XR_TAG_NULL:
            return XFB_TYPE_NULL;
        case XR_TAG_BOOL:
            return XFB_TYPE_BOOL;
        case XR_TAG_I64:
            return XFB_TYPE_INT;
        case XR_TAG_F64:
            return XFB_TYPE_FLOAT;
        case XR_TAG_PTR:
            return XFB_TYPE_OBJECT;
        default:
            return XFB_TYPE_OBJECT;
    }
}

// Record argument type at call site
static inline void xfb_record_arg(XmTypeFeedback *fb, int idx, XrValue val) {
    if (!fb || idx >= XFB_MAX_PARAMS)
        return;
    uint8_t flag = xfb_value_type_flag(val);
    fb->arg_types[idx] |= flag;
}

// Record return value type
static inline void xfb_record_return(XmTypeFeedback *fb, XrValue val) {
    if (!fb)
        return;
    uint8_t flag = xfb_value_type_flag(val);
    uint8_t prev = fb->return_type;
    fb->return_type = prev | flag;

    fb->sample_count++;

    // If no new type bits appeared, increment stability counter
    if ((prev | flag) == prev) {
        if (fb->stable_count < XFB_STABLE_THRESHOLD) {
            fb->stable_count++;
            if (fb->stable_count >= XFB_STABLE_THRESHOLD) {
                fb->stable = true;
            }
        }
    } else {
        fb->stable_count = 0;
        fb->stable = false;
    }
}

/* ========== API (implemented in xtype_feedback.c) ========== */

// Allocate and initialize a new TypeFeedback (zero-filled)
XR_FUNC XmTypeFeedback *xfb_create(void);

// Free a TypeFeedback
XR_FUNC void xfb_destroy(XmTypeFeedback *fb);

// Check if a specific arg_type is monomorphic (single type)
XR_FUNC bool xfb_is_monomorphic(uint8_t type_flags);

// Convert feedback bitmask → XrSlotType (for JIT/AOT specialization)
XR_FUNC uint8_t xfb_to_slot_type(uint8_t type_flags);

// Debug: print feedback summary
XR_FUNC void xfb_dump(const XmTypeFeedback *fb, int nparams, const char *func_name);

#endif  // XTYPE_FEEDBACK_H
